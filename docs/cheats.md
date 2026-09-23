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
  typed after the game starts. CoopIII compares the same way.
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
- **Nothing on screen says a cheat was refused.** The log does.

## 6. Checking it in the game

Not run in-game. Everything above is covered by `tools/clienttest` and
`tools/sessiontest` except the moment each handler actually runs.
