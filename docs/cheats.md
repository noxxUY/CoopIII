# Cheats in a session

What each GTA III cheat does in a co-op session, who owns what it changes, and
where it runs now. Measured against the retail 1.0 image on 2026-09-23;
`client/src/game/addresses.h` ("cheats") carries the instructions,
`client/src/game/cheats.h` the decisions, `docs/roadmap.md` §5.14 the switch.

---

## 1. There is one door, and it is the keyboard

Every cheat in 1.0 comes through `CPad::AddToPCCheatString` (`0x00492450`,
`__thiscall`, `ret 4`). Its only caller is the key-down handler's default arm
(`0x005841C7`), which runs off the window procedure on the same thread as
`CGame::Process`. The pad-button path re3 documents is not in this build:
`CPad::DoCheats(int16)` at `0x00492F20` is `sub esp,8 / mov [esp+4],ecx /
add esp,8 / ret 4`.

The function shifts the key into `KeyBoardCheatString` (`0x00885B90`, 20 bytes,
newest first) and then runs **23 independent `strncmp`s**, each followed by a
call to its handler. Not an else-if chain, so one key could fire two cheats; no
retail string is a suffix of another, and `tools/clienttest` types every one
after every other to prove it.

KANGAROO and PEDDEBUG, which re3 lists, are not in 1.0.

### 1.1 Two things re3 would have got wrong

- **BOOOOORING is broken in 1.0, and stays broken.** Its row pushes `10h`
  (`0x004925D6`) for a ten-letter string. `strncmp` goes on past the string's
  NUL and compares it against `KeyBoardCheatString[10]`, which only matches
  while that byte is still the zero the buffer starts with in `.bss`. Nothing
  else writes the buffer. So slow motion works only as the very first thing
  typed after the game starts. CoopIII compares the same way (and where
  SilentPatch has fixed it, as fixed: §8).
- **NASTYLIMBSCHEAT does nothing.** Its handler toggles `CPed::bNastyLimbsCheat`
  (`0x0095CD44`), and a byte scan of the whole image finds that byte written by
  the handler and by `CPad::ResetCheats` and read by nothing.

`tools/clienttest` found the first one. Given the exe (`COOPIII_GTA3_EXE`), it
decodes all 23 rows out of the function's bytes and compares them with the
table, and the first run disagreed on exactly that row.

## 2. The table

Class: **(a)** only the typing player's own state; **(b)** world state this
machine owns; **(c)** state another machine owns. "Where" is the decision under
the default `cheats = shared`.

| # | Typed | Handler | What it writes | Class | Where |
|---|---|---|---|---|---|
| 0 | GUNSGUNSGUNS | `0x00490D90` | `GiveWeapon` x11 on `FindPlayerPed` | a | here |
| 1 | IFIWEREARICHMAN | `0x00491430` | `Players[PlayerInFocus].m_nMoney += 250000` | a | here |
| 2 | GESUNDHEIT | `0x00490E70` | ped health 100; the car the player is **in**: health 1000, engine status 0 | a, c as a passenger | here, car half kept only for the driver |
| 3 | MOREPOLICEPLEASE | `0x00491490` | `SetWantedLevel(min(level + 2, 6))` | a | here |
| 4 | NOPOLICEPLEASE | `0x004914F0` | `SetWantedLevel(0)` | a | here |
| 5 | GIVEUSATANK | `0x00490EE0` | `new CAutomobile(Rhino, MISSION_VEHICLE)` near the player, unlocked | b | here |
| 6 | BANGBANGBANG | `0x00491040` | `BlowUpCar(nil)` through vtable slot 29 on every pool slot | c | here; refusals hold, wrecks travel |
| 7 | ILIKEDRESSINGUP | `0x004910B0` | random model, `SetModelIndex` | a | here |
| 8 | ITSALLGOINGMAAAD | `0x004911C0` | `CPedType[4..20].m_threats = 0xFFFFF` | c | everybody |
| 9 | NOBODYLIKESME | `0x00491270` | `CPedType[4..20].m_threats \|= PLAYER1` | b | here |
| 10 | WEAPONSFORALL | `0x00491370` | toggles `CPopulation::ms_bGivePedsWeapons` | b, per machine | everybody |
| 11 | TIMEFLIESWHENYOU | `0x004913A0` | `ms_fTimeScale *= 2` while < 4 | c | everybody |
| 12 | BOOOOORING | `0x004913F0` | `ms_fTimeScale *= 0.5` while > 0.25 | c | everybody |
| 13 | TURTOISE | `0x00491460` | armour 100 | a | here |
| 14 | SKINCANCERFORME | `0x00491520` | `CWeather::ForceWeatherNow(SUNNY)` | c | the host |
| 15 | ILIKESCOTLAND | `0x00491550` | `ForceWeatherNow(CLOUDY)` | c | the host |
| 16 | ILOVESCOTLAND | `0x00491580` | `ForceWeatherNow(RAINY)` | c | the host |
| 17 | PEASOUP | `0x004915B0` | `ForceWeatherNow(FOGGY)` | c | the host |
| 18 | MADWEATHER | `0x004915E0` | toggles `gbFastTime`: `CClock::Update` ticks a minute every frame | c | everybody |
| 19 | ANICESETOFWHEELS | `0x00491610` | toggles `CVehicle::bWheelsOnlyCheat` (drawing only) | a | here |
| 20 | CHITTYCHITTYBB | `0x00491640` | toggles `CVehicle::bAllDodosCheat` | a | here |
| 21 | CORNERSLIKEMAD | `0x00491670` | toggles `CVehicle::bCheat3` | a | here |
| 22 | NASTYLIMBSCHEAT | `0x004916A0` | toggles a byte nothing reads | none | here |

## 3. The decisions, one by one

**Weapons, money, health, armour, the skin (0, 1, 2, 7, 13).** The typist's
own. Weapons and health ride the snapshot, the skin rides `C_PlayerModel`,
money never travels. Run where typed.

**GESUNDHEIT's car half.** The handler heals `FindPlayerVehicle()`, which is
the car the player is *in*, passenger seat included. Healing somebody else's
car is an observer deciding its condition until that driver's next snapshot
takes it back. The handler still runs, so the player's health and the help text
are the engine's. If we are not the driver, the car's health and engine status
are put back straight after.

**The stars (3, 4).** A player's stars are their own machine's
(`docs/wanted.md` §4.1). `PlanWanted` already reads a level the engine reached
by itself as the player's own, so the cheat needed nothing. Under `wanted =
shared`, raising yours raises everybody's floor, and clearing yours leaves the
session holding whatever everybody else earned. Under `wanted = off` the level
goes back to zero on the next tick. Both are the wanted rule doing its job.
The earlier measurement that MOREPOLICEPLEASE adds exactly two stars holds:
`add eax,2 / cmp eax,6` at `0x004914B7`.

**The tank (5).** A `MISSION_VEHICLE`, so `IsAmbientCarWeShouldHost` skips it
and it stays on the typist's machine until somebody gets in. Then it is claimed
through `C_EnterVehicle` like any car in the street; the claim path has no
created-by filter. The cannon fires only on the driver's machine
(`TankControl` returns unless `this == FindPlayerVehicle()`, `0x0053D5EA`) and
its shell is `AddExplosion(nil, FindPlayerPed(), 8, ...)`, which
`game/combat.cpp` relays. `BlowUpCarsInPath` runs on every machine's copy, but
it destroys through slot 29 (`0x0053E06E`), so each car it crushes is decided
by whoever owns that car.

**BANGBANGBANG (6).** The handler walks the vehicle pool and calls slot 29 on
every slot. A byte scan finds six vtables in the image whose slot 29 is one of
three bodies: `CAutomobile::BlowUpCar` and `CBoat::BlowUpCar`, both detoured,
and an empty base shared by `CHeli`, `CPlane`, `CTrain` and `CVehicle`. So the
cheat reaches only the two detours, which decide on the car rather than the
caller:

| the car | verdict |
|---|---|
| the one we are driving | blows, goes out as `C_VehicleBlowUp` |
| a session car another player drives (protocol 23) | refused |
| a car another player is settling (24, 30) | refused |
| a replica of another machine's traffic (28) | refused |
| a parked car, a session car nobody holds, our own traffic | blows, goes out as an unowned wreck |

The refusals hold. What did not hold was **propagation**. Both queues of
unowned wrecks held eight and dropped the oldest. The traffic poll marks a car
reported before it queues it, so a dropped wreck was never tried again. The
receiver held 32 pending wrecks. One BANGBANGBANG is up to 110 wrecks in one
frame, so most of them stayed intact on every other screen. Both send queues
are now `game/wreckqueue.h`, sized to the vehicle pool (110, `push 6Eh` at
`0x004A17D5`) and to `MAX_HOSTED_CARS`. The receiver holds 128.

It is not routed. Sending it to everybody would blow up other players' cars
with them inside, which friendly fire off exists to prevent.

**The riot (8) and NOBODYLIKESME (9).** Nothing reads the `CPedType` table when
a pedestrian decides. `CPed`'s constructor copies it into `m_fearFlags`
(`+0x188`, at `0x004C4CD4`) and `CPed::ScanForThreats` (`0x004C5FE0`) reads
that copy. So a threat cheat changes everybody constructed afterwards,
**CoopIII's replicas and remote players included**: they are all built
through the same `CCivilianPed` constructor. And a replica acts on it:
`CivilianAI` (`0x004C07A0`) only tests `bRespondsToThreats` when the objective
is set, and a replica's objective is NONE, so it gets the full reaction. That
means flee, or `SetObjective(KILL_CHAR_ON_FOOT)`. That broke the rule that an
observer never decides what a remote entity does. It was reachable without a
cheat too, since `RegisterThreatWithGangPeds` ORs attackers into neighbours'
fears. `ScanForThreats` is now detoured to answer 0 for a remote player or a
replica pedestrian.

With that closed, the riot is the table on each machine turning that machine's
own crowd, so it goes to everybody. NOBODYLIKESME ORs in `PED_FLAG_PLAYER1`,
and PLAYER1 is the local player on every machine. Sent to everybody, it would
set every crowd on its own player instead of on the typist. Kept here, the
typist's own crowd turns on the typist, which is what was typed. Neither is
undone by `CPad::ResetCheats`, in single player either.

**WEAPONSFORALL (10).** Read only by `CPopulation::AddPed` (`0x004F532B`), so
it arms the pedestrians the typist's own generator makes. Run everywhere, so
every generator does, carrying the resulting state.

**The clock (11, 12, 18).** The time scale and `gbFastTime` are each machine's
own, and the clock's minutes come out of them. On a non-host these fought the
host's clock by a jump every second. On the host they dragged everybody after
it the same way. Run everywhere, carrying the resulting scale (a power of two,
`state` 0..4) or flag. A receiver steps there with the engine's own handlers.
A scale no cheat could have made (a mission's) is not stepped from.

**The skies (14-17).** `ForceWeatherNow` on a non-host lasted until the next
`S_WorldState`, at most a second. They go to the host, which runs them and
sends its world packet at once. A non-host does not run them locally: its sky
would only show the new weather until the in-flight world packet put the old
one back.

**The handling toggles (19, 20, 21).** Machine-wide, but every car they reach
on the typist's machine is either the typist's own, whose transform travels,
or somebody else's replica, which is corrected every frame, or the typist's
own traffic. Run where typed.

## 4. The switch

`cheats = shared | personal | off` in `CoopIII-Server.ini`, default `shared`.
It sits in `S_Welcome.flags` bits 6-7. The server refuses to relay what the
rule refuses, and the client refuses to run or apply it.

| rule | the 13 personal | BANGBANGBANG | the 4 skies | the 5 everybody-cheats |
|---|---|---|---|---|
| `shared` | here | here | to the host | here and everybody |
| `personal` | here | refused | refused | refused |
| `off` | refused | refused | refused | refused |

A refusal is one log line naming the setting:
`cheats: PEASOUP refused - it changes the world and ...`.

A joiner is handed the last state of each everybody-cheat. The skies come
with the host's world packet anyway. An emptied session forgets them.

## 5. What this does not do

- **Leaving a session keeps what its cheats did.** A riot, a time scale and an
  armed crowd stay on the machine, as they do in single player until a load.
- **A load mid-session resets this machine's** time scale and toggles through
  `CPad::ResetCheats` and nobody else's.
- **A replica still runs the rest of `CivilianAI`.** Only the threat scan is
  closed. The replicas' positions are the stream's either way.
- **Nothing on screen says one of the game's cheats was refused.** The log
  does. CoopIII's own say why in the chat feed (§7).

## 6. Checking it in the game

Not run in-game. Everything above is covered by `tools/clienttest` and
`tools/sessiontest` except the moment each handler actually runs.

## 7. CoopIII's own cheats

CoopIII has typed cheats of its own, typed in play the way the game's are, with
no Enter. `client/src/game/cheats.h` holds the table (`COOP_CHEATS`): a word
in capitals and, for one that takes it, one digit after it.
`client/src/game/tpto.h` decides what the first one does and
`client/src/game/tpto.cpp` does it.

| Typed | What it does |
|---|---|
| `TPTO1` .. `TPTO8` | puts you beside the player with that number |

**The number is the one the Tab list puts before every name**, the slot plus
one (`chatfeed.h`, `ListNumber`). `/kick` takes the same number. TPTO9
and TPTO0 are nobody.

**How they are read.** Nothing is hooked for them. After every key-down the
game's own window procedure has handled, `game/chat.cpp` hands the engine's
`KeyBoardCheatString` to `NoticeTypedKeys`. When it has changed and now ends
in one of ours, the cheat is queued, and `TickCoopCheats` does it on the next
frame, before `CGame::Process`. This works whether or not the
`AddToPCCheatString` detour is in, and it only sees the digit row: the
handler turns the numeric keypad into codes of its own, 0x400 and up
(`VK_NUMPAD0` is `mov [ebx],40Eh` at `0x00583C61`), which it never pushes.
TPTO starts with the chat key, so it is nearly always finished through the
chat line (§8).

**On foot** it is the move the rampage vote and the session's mission already
make (`rampagevote.h`, `MovePlayerBeside`). That is out of any car another
player sits in, on the ground near his, in sight of him and facing him, with
`CStreaming::LoadScene` round the spot first. For another island the player is
held where the target is until `CCollision::Update` has loaded it, then put
down.

**At the wheel the car comes along.** That is `SET_PLAYER_COORDINATES`' car
half, the car's own `Teleport` through vtable slot 11 at the ground plus
`GetDistanceFromCentreOfMassToBaseOfModel`. Only a `CAutomobile`, whose slot
11 is `CAutomobile::Teleport` (`0x00535180`): it takes the car out of the
world, puts it there level and still, resets the suspension and adds it back.
The spot is the first of eight directions on a 7 m ring, then a 10 m one,
with ground near the target's, in sight of him, and nothing in the car's
bounding sphere (`CWorld::TestSphereAgainstWorld`). The script's
`ClearSpaceForMissionEntity` is left out because it makes room by deleting
cars, and the car beside a player is as likely as not somebody's session car.

**What it refuses**, each with one line in the chat feed:

| Why | Line |
|---|---|
| the server's `coopCheats = off` | TPTO is switched off on this server |
| on a mission (a rampage counts), unless `coopCheats = always` | TPTO: not during a mission |
| no player with that number | TPTO: nobody is number 5 |
| your own number | TPTO: number 1 is you |
| busted, being arrested | TPTO: not while busted |
| wasted | TPTO: not while wasted |
| a cutscene | TPTO: not during a cutscene |
| already being moved | TPTO: already on the way |
| nothing has said where he is yet | TPTO: nobody knows where bob is yet |
| his island is not open in your story | TPTO: bob is on an island your story has not opened |
| a passenger | TPTO: get out of the car first |
| a boat, a plane, a heli, a train | TPTO: cannot take this vehicle along |
| driving, and he is on another island | TPTO: bob is on another island, leave the car first |
| driving, and no empty spot for the car | TPTO: no room for the car beside bob |

On success the game's own "Cheat activated" line comes up, through the same
call its handlers make: `CHud::SetHelpMessage(TheText.Get("CHEAT1"), true)`,
with the key at `0x005F64C0`. With no session TPTO does nothing and says so
only in the log, and anything typed with the pause menu up is dropped.

**The switch.** `coopCheats = outsidemissions | always | off` in
`CoopIII-Server.ini` and the server's options, default `outsidemissions`. It
is not `cheats`, which is about the game's 23. It travels in
`S_SessionRules.coopCheats` (protocol.md 1.61). The server cannot refuse a
teleport, which is the player's own position going out as it always does, so
each client keeps the rule itself.

**Adding one.** A `CoopCheatId` before `COOP_CHEAT_COUNT`, its row in
`COOP_CHEATS`, and a case in `tpto.cpp`'s `RunCoopCheat`. `tools/clienttest`
types every row past the chat key and checks that no word ends one of the
game's 23, or is ended by one, which would fire both on one key.

## 8. The chat key, and a game another plugin has changed

**Why typing a cheat with a T in it still opened the chat in noxx's game.**
The rule that rescues a cheat from the chat line (`cheats.h`, "the chat key
inside a cheat") compared what was typed against `CHEAT_SITES`, which is
retail 1.0. noxx's game runs SilentPatch III, and reading the two running
games' `CPad::AddToPCCheatString` on 2026-09-30 showed it rewrites two rows:

| Row | Retail | SilentPatch |
|---|---|---|
| 12 BOOOOORING | `push 10h` (`0x004925D6`) | `push 0Ah` |
| 13 armour | `push 5F6618h`, "TURTOISE" | `push 61E98EF4h`, its own "TORTOISE" |

In that game TORTOISE is the armour cheat and TURTOISE is nothing. Typed with
T as the chat key, TORTOISE was never recognised: the line stayed open and
the keys never reached the game. The same mismatch is what every log there
says at start, `row 12 (BOOOOORING) of CPad::AddToPCCheatString is not the
one CoopIII has on record`. So the detour is never installed there, and none
of §2's routing runs in that game. The chat-line rule did not depend on the
detour, so that alone did not break it.

**Now** the rule goes by the table the running code compares
(`TypedCheatTable`). At start `game/cheats.cpp` decodes the 23 rows out of the
code in memory with the same decoder `tools/clienttest` runs over the exe,
and reads each string where its `push` points, after checking with
`VirtualQuery` that it can be read. It logs every row another plugin has
changed. If the code is not in the shape `addresses.h` has, it falls back to
retail's table. CoopIII's own cheats are part of the same check, and so is
the digit row. After every key typed into an open line, if the chat key and
the line finish a cheat, the line is shut at once, unsent, and the keys go to
the game. If they do not, it stays chat. `tools/clienttest` types TURTOISE,
ITSALLGOINGMAAAD, NOBODYLIKESME and TPTO3 straight through, and both
spellings against both tables. It also checks that a line starting with T, or
one about TPTO3, is sent whole.

**The detour now goes in there too.** Its check used to want retail's table
row for row, so in a game with SilentPatch it stayed out and the game's
cheats ran as in single player. Now `CheckCheatImage` (`cheats.h`) checks
only what the detour depends on, on the code that is running:

- the prologue it moves aside (`CPAD_ADD_TO_PC_CHEAT_STRING_PROLOGUE`) and the
  shift it does itself (`CPAD_CHEAT_SHIFT`, `0x00492457`, 30 bytes);
- every row's shape and handler, which must be retail's;
- each row's pushed length and the string bytes its `strncmp` can reach (up
  to the length, or a shared NUL), which must be retail's or one of
  `KNOWN_CHEAT_ROWS`;
- the epilogue's `ret 4`.

`KNOWN_CHEAT_ROWS` has SilentPatch's two: BOOOOORING with length 10 at
retail's string, and the armour row with length 8 pointing at "ESIOTROT"
anywhere, since its address is wherever SilentPatch's module was loaded.
Anything else (a third rewrite, a different length, another string, a changed
handler, a hooked prologue, a string that cannot be read) and the detour stays
out as before. The log names the row, and says for each changed row whether
it is a rewrite CoopIII knows.

Once in, the detour compares with the table read off the running code, not
`CHEAT_SITES`. So in a session in that game TORTOISE is armour and TURTOISE
is nothing, and BOOOOORING works after other keys, as in that game's single
player. The rows are matched by position, which is the `CheatId` on the wire,
so nothing on the wire changes. The log lines name each cheat the way that
game spells it.

`tools/clienttest` builds the function byte by byte in both shapes (the
retail one checked against the exe, byte for byte), and checks that both go
in, that each of the refusals above stays out, and what the detour then
matches.

**Not proven:** none of §2's routing has run in a game with SilentPatch yet.
It needs a session there: the log should say the function was hooked, and the
sky cheats should go to the host.
