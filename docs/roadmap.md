# Roadmap

The goal: the GTA III campaign, played co-op, with the game otherwise 1:1
faithful. This document is the path from where we are to that, and the list of
things that have to be true along the way.

Read `protocol.md` for the wire contract, `campaign.md` for the mission design
and `compat.md` for the constraints the player's other mods impose. This file
is the *plan*; those are the *decisions*.

---

## 1. Where we actually are

Verified running inside retail GTA III v1.0 on the target machine:

| | |
|---|---|
| Image guard | Refuses to load against any other build. ✅ |
| Frame hook | `CGame::Process` detoured; verified by reading the `E9` in live memory. ✅ |
| Socket thread + queues | Connected, 2 ms RTT on loopback, never touches game memory. ✅ |
| Roster | Join/leave/backfill, self-echo rejection, two-phase spawn. ✅ |
| Local player sampling | Position, heading, velocity, health, armour, ped state and move state, read from real memory every 25 Hz tick. ✅ Animation (base + partial, with phase), held weapon and aim yaw/pitch were added for M1 and are sampled from verified offsets, but have not been through a live session yet. |
| Remote ped creation | Created via the engine's own `CREATE_CHAR` path. Survived 18 000 frames. ✅ |
| Remote ped **rendering** | ✅ Fixed. Remote players are now visible in-game. |

---

## 2. The structural problems

None of these are bugs. It is how a 2001 single-player engine works, and it
shapes every milestone below. Meeting them early is cheaper than running into
them at milestone 5.

### 2.1 The streamer centres on one player

```cpp
StreamZoneModels(FindPlayerCoors());   // re3 src/core/Streaming.cpp:346
```

The game loads models and collision around *the* player, singular. Two players
a few hundred metres apart are fine. Two players across the map are not: the
remote one is standing in geometry that, on your machine, isn't loaded.

Consequences to design for:
- A remote player outside your streaming radius has no model to render.
- They will fall through the world if collision is not loaded there.
- Naively requesting models for every remote player will thrash the streamer
  and blow the memory budget the game was tuned for.

Options, none free: keep players soft-tethered; stream a reduced set around
remote players; or accept that distant players are represented by a blip only.
This needs a decision before milestone 2, because vehicles make players separate
much faster than legs do.

### 2.2 Only one island's collision is in memory

```cpp
CGame::currLevel;                                        // Game.h:15
CModelInfo::RemoveColModelsFromOtherLevels(currLevel);   // Game.cpp:651
CCollision::ms_collisionInMemory = currLevel;            // Game.cpp:652
```

`eLevelName` is `INDUSTRIAL` / `COMMERCIAL` / `SUBURBAN`, and it is a single
global. So two players on different islands is not a distance problem at all.
The other island does not exist in this process.

The campaign gates islands behind story progress, so in a co-op campaign
everyone is usually on the same island, which saves us. Free roam has no such
guarantee, though, and the bridge sequences deliberately move players between
islands.

### 2.3 There is exactly one player slot

`NUMPLAYERS = 1` (`config.h:7`). Remote players are ordinary `CPed`s, never
`CPlayerInfo` entries (`protocol.md` §1.3). Everything that reaches for
`CWorld::Players[0]` is local-player-only *by definition*: the wanted level,
the camera, the HUD, the script.

### 2.4 Physics is not reproducible across machines

`ms_fTimeStep` is frame-time-derived (`protocol.md` §1.2). There is no fixed
timestep, so the same inputs do not produce the same outputs on two machines.
Lockstep and rollback are both out. Hence the client-authoritative design with
a host-authoritative script (`campaign.md` §2).

---

## 3. Milestones

### M1 - See each other *(current)*

- [x] Fix remote ped rendering. Remote players are now visible in-game.
- [x] Animation sync: `animId` + time, base and partial, plus the held weapon
      and aim yaw. Built and unit-tested, but it still needs an in-game run to
      confirm it looks right. Protocol 2. See `protocol.md` §1.8 for the wire
      format and `client/src/game/addresses.h` for the offsets it reads.
- [x] Nametags over remote players. Weapon icon, name, health, drawn with the
      game's own `CFont` and its own `hud.txd` sprites from a detour on
      `CHud::Draw`. Fade out by 50 m and fade out behind walls, the second
      through one `CWorld::GetIsLineOfSightClear` per frame shared round robin
      across the roster. Run in game 2026-09-22 and the size settled at
      nametagScale 1.35. See
      `client/src/game/nametag.h` for the design and the numbers.
- [x] Remote players on the minimap, as the rotating arrow the engine draws
      the local player with — so a team-mate reads as a person facing a
      direction rather than as a coloured square. Drawn from a detour on
      `CRadar::DrawBlips`, after the original, out of the engine's own
      `CentreSprite` and its own four-step blip preamble
      (`TransformRealWorldPointToRadarSpace`, `LimitRadarPoint`,
      `CalculateBlipAlpha`, `TransformRadarPointToScreenSpace`). Green on foot
      and red in a car, which are `ADD_BLIP_FOR_CHAR`'s and
      `ADD_BLIP_FOR_CAR`'s own colours, fetched from
      `CRadar::GetRadarTraceColour` rather than written down. No protocol
      change. It went through `CRadar::ms_RadarTrace` first and the table is
      gone: the square is the one thing a table entry cannot stop being. See
      `client/src/game/radar.h`. Not yet run in game.
- [x] Decide the streaming policy (§2.1). Settled, see §5.3.

Done when: two players can see each other walk around Portland and it looks
right.

### M2 - Vehicles

**Unblocked 2026-09-21.** M2 was gated on `CVehicle` offsets that had been
located and never verified; all 22 have now been proved against the retail
binary and moved into `client/src/game/addresses.h`, along with the engine's
own `CREATE_CAR` creation path. Start with the spawn, and mind the reference
registration and the double deletion it guards against: a vehicle has two
deletion gates (`!bIsLocked && CanBeDeleted()`), so the Area B trap is waiting
here in a slightly nastier form. `client/src/game/vehicle.cpp` and `vehicle.h`
are where that path lives.

**Started 2026-09-21.** `client/src/game/vehicle.*` and the client/server
roster are in. What works and what does not is in the checklist below.

**How a vehicle enters the session, decided 2026-09-21.** GTA III generates
its own traffic and parked cars, locally and differently on every machine, so
there is no shared vehicle world to refer to, and synchronising Liberty City's
whole car population is not on the table. So a car joins the session when a
player first gets into it. The client sends `C_EnterVehicle` with the car's
identity and `netId == INVALID_NETID`, the server allocates a netId and tells
everyone else to create a matching one, and the reply tells the claimer what
its own car is called. Cars nobody has touched stay local and unsynchronised:
two players see different traffic and neither can tell. Leaving a car does not
remove it. Somebody parked it, it is still there.

- [x] Vehicle spawn/despawn, driver-authoritative.
- [x] Transform + velocities at 25 Hz, full rotation as a quaternion
      (`client/src/quat.h`, round-tripped in `basetest` over orientations a
      yaw-only sync would lose).
- [x] Steering, throttle, brake, gear, engine state, siren, lights.
- [x] Interpolation, with a slerp for the rotation (`VehicleInterpBuffer`).
- [x] Simulate and correct: the transform is written after `CGame::Process`,
      every frame, so local physics cannot own a car somebody else is driving.
- [x] Seat the remote driver. `SeatRemotePed`/`UnseatRemotePed`
      (`client/src/game/ped.cpp`), driven live from `UpdateRemoteSeats`
      (`client/src/client.cpp`). A remote driver now sits in their car instead
      of standing where it is.
- [x] Play it through the engine's own API. `CPed::SetEnterCar` opens the
      door and `CPed::SetExitCar` closes it, both verified against the retail
      image (`addresses.h`). The warp is still there and is still what
      guarantees the seat: the animation is an attempt with a deadline, and
      every way it can end short - refused, interrupted, the car driving off,
      the player dying, the clock running out - falls back to
      `WarpPedIntoCar` in the same frame. `CPed::SetCarJack` is recorded and
      deliberately not called; see docs/protocol.md §1.14.
      **Not yet run in the game.**
- [x] Tell the session an entry has *started*, and which door it goes in
      through. `C_EnteringVehicle`/`S_EnteringVehicle`, a statement of intent
      that claims nothing: the claim that names the car and moves ownership
      is still the `C_EnterVehicle` at the end. The door matters because the
      seat does not name it - `CPed::SeekCar` walks a driver to the *nearest*
      door via `CPed::GetNearestDoor` (`0x004E1CF0`) and the engine shuffles
      him across the front seats inside the car, so pressing the enter key on
      the passenger side and being seen to teleport to the driver's door was
      one observer opening the wrong one. docs/protocol.md §1.14.7.
- [x] Shut a door an abandoned entry left open. `CPed::QuitEnteringCar`
      touches the car's flags and not its doors, so an entry given up on
      after the door-opening animation left that car with a door hanging open
      and nobody in it. `AbandonPedEnterCar` now makes the engine's own
      closing call. docs/protocol.md §1.14.8.
- [x] Passengers: several players in one car, with the driver owning physics.
      A key the original game has no binding for (`seatKey`, G by default)
      puts the local player in the first free passenger seat of the nearest
      of the session's own cars. The engine picks the slot - `WarpPedIntoCar`
      takes the passenger arm for any objective that is not
      `ENTER_CAR_AS_DRIVER` - and CoopIII reads back which one it got,
      because that number is what the session is told. Getting out is the
      game's own exit key; there is no CoopIII way out, and the client
      notices the seat is empty. `client/src/game/seat.h`.
- [x] Same car on every screen: colours **and extras**. `EnterVehicleBody` and
      `S_VehicleSpawn` carry `m_aExtras`, forced on the receiving machine
      through `CVehicleModelInfo::ms_compsToUse` before the constructor runs,
      because they cannot be applied after it. `protocol.md` §1.12.
- [x] Ownership handoff when the driver changes. Decided and built: §5.8.1
      is the rule (the player in seat 0 owns it, and between an exit and the
      next enter nobody does) and §5.8.2 the two halves that were missing.
      What this line said before that, kept for the history: it had a
      **stopgap in front of it**: `CorrectRemoteVehicle` stops
      correcting a car the local player is sitting in the driver's seat of.
      Without that, a car another player claimed is a car you can get into and
      cannot drive an inch, because the session's transform is written over
      yours sixty times a second. The session still thinks the other netId
      names it and the local claim makes a second one; the guard only buys
      time. `protocol.md` §1.11.5.
- [x] The car blows up on every screen. `C_VehicleBlowUp` / `S_VehicleBlowUp`
      (0x36/0x37), decided by the driver's machine, replayed by every observer
      through the engine's own `CAutomobile::BlowUpCar` at the owner's
      transform. `protocol.md` §1.11.
- [x] **Vehicle damage short of destruction: panels, doors, lights, wheels.**
      Built since, as panels and doors on a change-only reliable packet
      (version 18, [cardamage.md](cardamage.md)); not run in the game. The
      notes below are from before it, when it was deliberately left out of
      the destruction work and the addresses proved while doing that were the
      head start:

      | Symbol | Address / offset | How it was proved |
      |---|---|---|
      | `CVehicle::m_damageManager` (`CDamageManager`) | `+0x288` | `BlowUpCar`'s `lea ecx,[ebx+288h]`; also `== SIZEOF_VEHICLE`, i.e. `CAutomobile`'s first member |
      | `CDamageManager::FuckCarCompletely` | `0x00545B70` | the call `BlowUpCar` makes on that sub-object |
      | a `CAutomobile` panel/door setter | `0x00530120` | `BlowUpCar` calls it four times with `(id, status, 0)` triples - `(7,5,0)`, `(8,6,0)`... It takes the **vehicle**, not the damage manager: `lea ecx,[ebp+288h] / call 0x005458E0` records the damage, then `[ebp+ebx*4+37Ch]` reaches what is almost certainly `m_aCarNodes` and hides the frame. Both halves have to happen or the panel stays on screen |
      | `CDamageManager` engine-status setter | `0x00545940` | `mov [ecx+4], min(arg, 0FAh)`, a saturating byte write. `CVehicle::InflictDamage`'s "set on fire" arm pushes `0E1h` (225) into it, so `m_engineStatus >= 225` is what "this car is burning" means. `+4` is the engine byte and 250 is its cap |
      | `CDamageManager` layout, partially | `+5`, `+9`..`+0E`, `+4` | `0x00545B70` writes 2 into `+5` and 3 into the six consecutive bytes `+9`..`+0E`, which is the wreck state for six separate parts |
      | `m_nTimeOfDeath` | `+0x210` | `BlowUpCar`'s `mov eax,[0x885B48] / mov [ebx+210h],eax` |
      | `bRenderScorched` | byte B (`+0x52`) bit 4 | `BlowUpCar`'s `and al,0EFh / or al,10h` |

      Three things a reader of those should know before building on them.
      Everything in the "how it was proved" column was read out of the binary
      on 2026-09-22 and the three function addresses are in
      `docs/addresses-unverified.md` rather than in `addresses.h`, because
      what has been read is their *shape* and their call sites, not their
      whole bodies matched against re3 - which is the standard the rest of
      `addresses.h` is held to. Do not promote them without doing that.
      Second: `m_aCarNodes` at `+0x37C` is a guess from one subscript and
      nothing else, and a wrong node array is a write into a neighbouring
      member rather than a crash. Third, and the design point: this is a
      *state*, not an event. Unlike a blast it belongs in the snapshot beside
      health, which means the snapshot grows and the wire changes again -
      whereas destruction deliberately did not touch the snapshot at all.
- [x] Boats. `SpawnRemoteVehicle` builds a `CBoat` for a boat model, chosen
      the way `CREATE_CAR` chooses (model info type and `+0x58`, not model
      ids). The car-only writers are guarded on `m_vehType`: the damage model
      is skipped both ways, and the fire timer is held on the boat's own
      member (`+0x2CC`, not `+0x530`). A replica is seated in a boat by
      warp, with the objective `CPed::SetObjective` takes away from
      non-players put back by hand. No wire change. `addresses.h`, "boats",
      and `game/boat.h`. Not run in the game yet.
- [x] Never use `STATUS_PLAYER_REMOTE`. It is RC-car mode and it detonates
      cars (`protocol.md` §1.4). Held to: nothing in the client writes it.
- [x] Run in the game. A car spawns, renders upright, survives in the pool
      and is corrected against local physics every frame. Not yet with a real
      second player: `ghost -car` is what has driven it so far.
- [x] A car changing hands between two other players is played on its new
      driver's clock. The buffer used to keep the old driver's samples, so a
      machine whose game had started later had every snapshot dropped as older
      than the last and the car stood still on every other screen for as long
      as the two games' start times were apart (`Client::OnReportersClock`,
      2026-09-24). No wire change. Not run in game.

Done when: one player drives, another rides, and it looks the same on both
screens.

### M3 - Combat

- [x] Weapon sync: current weapon, ammo, aim yaw/pitch. The weapon and both
      aim angles are sent and applied, pitch through the engine's own IK;
      ammo since version 18, behind the server's ammo switch. See the sync
      inventory.
- [x] Shot events (reliable), replayed through the real `CWeapon::Fire` so
      impacts happen for real (`combat.cpp`, `C_Shot`, `CH_EVENT`). Muzzle
      flash not separately handled.
- [x] Damage application and death, through `InflictDamage` / `SetDie` /
      `SetDead` rather than by writing health. **Run end to end in game
      2026-09-21**: the shooter's `C_Damage`, the victim applying it, the
      death and the respawn all appear in the two logs for the same hit. See
      `protocol.md` §1.10.2 for the exemption that makes it work, which is
      the whole fix.
- [x] Respawn. The engine's own, on the machine that died; what travels is
      that it happened, and observers see the ped die and come back. Run in
      game 2026-09-21. The clock jumping 12 hours on death comes free, since
      that is `CGameLogic::PassTime(720)` and the host's clock is the
      session's.
- [x] Arrest: police station, the same. The busted player's machine sends
      leaving `PED_ARRESTED` as `C_Respawn`, and a player a cop drags out of
      a car is taken out of the seat on the snapshot instead of at the end
      of the drag. No packet, no version. `protocol.md` §1.10.8. Built
      2026-09-23, not run in game.
- [x] Melee. The hit always travelled as damage; since version 36 it also
      says which fight path landed it and with what move, so the victim's own
      engine plays the defend, the knockdown or the shove, and copies never
      react to a melee hit (`client/src/game/melee.h`). Not run in game.
- [x] Decide friendly fire. Settled in §5.2: off by default, server option.

### M4 - The world

- [x] Pickups: weapons, health, armour, hidden packages. Collection is
      exclusive - the client detects, the server arbitrates, the engine
      awards, **in that order**, because a pickup reward cannot honestly be
      taken back. Designed and built 2026-09-22; not yet run in game.
      **[docs/pickups.md](pickups.md)** is the investigation and the design,
      `client/src/game/pickup.*` is the seam, protocol **11**.
      - The finding that shaped it: no spawn packet was needed. Every machine
        already runs `main.scm` and creates all 448 script pickups itself from
        literal coordinates, so the worlds already agreed and only the
        exclusivity was missing.
- [x] Ped drops: the money and weapons a dead ped leaves. Out of M4's pickup
      line deliberately - it is a *spawn* problem, not an exclusivity one. The
      positions and amounts come straight out of `CGeneral::GetRandomNumber()`
      and the ped is usually ambient, so it exists on one machine only
      (`pickups.md` §1). Built on `C_PickupDrop` / `S_PickupDrop`
      (0x86/0x87): the host reads back what its own engine created and every
      observer replays it (the sync inventory, "Pickups a dead ped drops").
- [x] Remote players' kills counting toward a rampage. The frenzy already
      started for everybody (§5.10); what did not work was the counting, and
      it was worse than "each machine counts its own". `CPed::InflictDamage`
      only reaches `CDarkel::RegisterKillByPlayer` when the damaging entity is
      `FindPlayerPed()` or `FindPlayerVehicle()` (the test at `0x004EAD1A`),
      so a pedestrian hosted here and shot from there was credited to
      **nobody** - the host saw a replica, and the shooter's own engine had
      returned before that line. Built 2026-09-23, not yet run in game.
      **[rampage.md](rampage.md)** is the investigation and the design.
      Kills ride the engine's own three arguments (victim model, weapon,
      headshot), the ending is arbitrated by the server, and
      `CDarkel::ReadStatus` - one caller in the whole image, script opcode
      `01FA` - is what holds every machine's `rampage.sc` on the same value.
      Vehicle rampages (`RegisterCarBlownUpByPlayer`) are the same three lines
      and are deliberately still open; `rampage.md` §7.
- [x] Doors and garages. Built 2026-09-22, not yet run in game.
      **[protocol.md §1.16](protocol.md)** is the design,
      `client/src/game/garage.*` is the seam, four opcodes in `0xA0..0xAF`.
      - The finding that shaped it: nothing has to be spawned. All 32 garages
        come out of `main.scm` at literal coordinates, so the worlds already
        agree - only `CGarage::Update` asking `FindPlayerPed()` was missing.
      - What travels is the **state transition**, never `m_fDoorPos`. The
        door's height is derived on every machine from the state, at its own
        frame rate, through the engine's own ramp - and it could not travel
        even if it were worth it, because it is read against a door `CEntity`
        that each machine resolves to its own pool pointer (§5.8's rule).
      - Authority is a **union**, not §5.8's first-report-wins. A garage
        belongs to the map so there is no owner, and a door is a *level*
        rather than an event: first-report-wins would mean a door that never
        closes, last-writer-wins would mean the first player to walk away
        shutting it on the second.
      - **The safehouse pedestrian door and the save point are NOT garages**
        and are not done. They are `main.scm` objects (`#PLAYERSDOOR`,
        swung by `034D ROTATE_OBJECT` from `save.sc`) gated on the local
        player standing in a box, so syncing them means running the script
        host-only or intercepting that opcode - M5, `campaign.md`. The save
        menu staying local is also the safe answer.
- [x] Pay'n'Spray: the repair and the repaint. The colours travel because the
      retail `ChooseVehicleColour` is a per-machine round robin rather than an
      RNG roll (§1.16.5) - same class as §5.9, third time, and re3 would have
      given the wrong *reason* for the same right answer. The wanted level
      does not travel; see the next line.
- [x] The wanted level a Pay'n'Spray clears. Left as a marked seam rather
      than a second mechanism, and §5.1 landed without it having to change:
      the player who paid gets their own stars cleared by their own engine,
      under `shared` their own `PlanWanted` works out what that does to the
      session, and an *observer* never clears its own player's stars
      (`protocol.md` §1.16.6).
- [x] Destructible objects, explosions, fires. Explosions are replayed at the
      owner's position, fires are world state (§5.7, all three phases), and a
      broken or knocked-over street object is shared (§5.13). See the sync
      inventory's World table for what has been run in the game. A window
      somebody else's drive-by hits breaks on every screen, through the
      engine's own `CGlass` call for that round (objects.md §8.4; the address
      is a lead checked at run time). Not run in game.
- [x] Wanted level. Per-player, GTA Online's vehicle rule, server-configurable
      (§5.1). Designed and built 2026-09-22; **not yet run in the game**.
      **[docs/wanted.md](wanted.md)** is the investigation and the design,
      `client/src/game/wanted.*` is the seam, and the wire cost is four spare
      bits of a flags byte that was already going out plus two in `S_Welcome`.
      - The finding that shaped it: **the police needed nothing.** A cop ped
        is a `RANDOM_CHAR` and a police car a `RANDOM_VEHICLE`, so both
        already passed population.md's host tests unchanged - the wanted
        player's own engine spawns them, `CCopPed` can only chase
        `FindPlayerPed()` so they chase that player, and every other machine
        has been receiving them as ambient replicas all along. Nobody had
        noticed because no machine had ever had a reason to make one.
      - The other one: crimes are already attributed correctly, because
        `CEventList::RegisterEvent` reports nothing unless the criminal is
        the local player's own ped. M3 replaying somebody else's shot here
        cannot raise this machine's stars.
      - **Except the helicopter.** From three stars (`CWanted::NumOfHelisRequired`,
        `0x004ADC00`: 3-4 stars one, 5-6 two) `CHeli::UpdateHelis` builds a
        `CHeli` as `PERMANENT_VEHICLE`, locked, that chases `FindPlayerCoors()`.
        population.md's host test refuses both, so only the wanted player's
        own screen had it. It has its own opcodes now (0xA4-0xAB): the
        machine that spawned it streams it, every other machine flies a
        replica, and hits and its gunfire travel (`protocol.md` §4).
- [x] Ambient peds and traffic: an ownership model, or accept divergence.
      `protocol.md` §3 accepted divergence for v1, which was honest then and
      wrong for a finished mod. **Designed 2026-09-22 in [population.md](population.md)**:
      whoever's engine made it owns it, caught at `CWorld::Add`. **Built and
      measured in a live session 2026-09-22 for pedestrians**: two machines,
      eleven pedestrians between them, and both engines counting eleven rather
      than twenty-two. The counter rewriting the design called for turned out
      to be unnecessary - `CPed::CPed` already counts a replica the moment it
      is constructed, so the generator was seeing the shared crowd all along
      (`population.md` §1.3.1). Traffic was step 4, and the same measurement
      had to be repeated for it because `CCarCtrl`'s counters are not
      maintained the same way; it was, the same day, with the same result as
      long as the replica is put in the right counter (`population.md`
      §1.3.2).

### M5 - Campaign, Tier 2

The design is settled in `campaign.md`; this is implementation.

- [ ] Suppress `CTheScripts::Process()` on clients.
- [ ] Settle what *else* the main script drives that clients would lose:
      pickups, garages, save points, the intro (`campaign.md` §6). Do this
      first, it may change the shape of the rest.
- [ ] Replicate script effects: subtitles, blips, script spheres, mission
      state, cutscenes, fades.
- [ ] Mission entities as ordinary netid entities.
- [ ] Mission cleanup on pass/fail.
- [ ] Cutscene handling: freeze remote players too, or they wander through the
      scripted camera.
- [ ] Server options for the three deliberate SP divergences (`campaign.md`
      §2.3): death fails the mission, cutscene freezing, host-owned save.

Done when: a group can play a mission from start to finish together.

### M6 - Campaign, Tier 3

- [ ] Intercept the three trigger opcodes so *any* player can start a
      mission, not only the host. Measured over the real campaign: 123 call
      sites across `IS_PLAYER_IN_AREA_3D` (63), `IS_PLAYER_IN_AREA_2D` (57),
      `IS_PLAYER_IN_ZONE` (3). `campaign.md` §4.
- [ ] Decide: one detour on `ProcessCommands` switching on opcode, or patch
      the individual handlers.

### M7 - Everything that makes it usable

- [x] Chat on the HUD. T opens a line, Enter sends it through the `C_Chat`
      the protocol always carried, and the last ten lines sit above the radar
      for ten seconds each, every name in its player's colour and a long
      message broken over as many lines as it takes (`client/src/chatfeed.h`,
      `game/chat.cpp`). The line being typed has a caret the arrows, Home and
      End move, Up and Down go back through the last ten lines sent, Ctrl+V
      or Shift+Insert pastes and Shift+Delete empties it. An accented letter
      is typed as its plain one, because CFont's `FONT_BANK` has no glyph for
      it. The keys are read off the game's own window, and while a line is
      open the controls are held through the same `DisablePlayerControls` bit
      the menu uses. Both keys are settable (`chatKey`, `listKey`). Built
      2026-09-23, not run in game.
- [x] The version in the bottom-left corner, under the radar, grey and small:
      "CoopIII 0.0.1", from `sdk/include/coopiii/version.h`, which basetest
      holds to `set_version` in `xmake.lua` and to the installer's component.
      Also the first line of both logs. `showVersion = false` in CoopIII.ini
      hides it.
- [x] Player list on screen: F9 shows who is in the session, with their
      health, whether they are dead or in a car, and their stars.
- [x] Ping on screen, in the F9 list beside every name, yours included. The
      server broadcasts each slot's round trip as ENet measures it once a
      second (`S_PlayerPings`, 0xB2), so every machine shows the same number
      for everybody. Not a version: an older client drops the opcode.
- [x] Join/leave messages in game, and a line when the connection to the
      server is lost. A join is only announced for somebody arriving now,
      not for everybody a joiner is told about (`PJF_ARRIVED`, not a
      version).
- [x] Reconnection that restores you where you were. The network thread
      reconnects by itself every two seconds after a loss, and the game on
      the player's own machine never stopped, so where they are, what they
      carry and their stars are all still theirs; the HUD says "lost the
      connection" and then "back in the session". The one thing that used to
      come back wrong was the car they were sitting in: the old session
      handed it to their engine, the backfill then built a second copy on top
      of it, and their claim named the one they were in as a new car. Now the
      car is remembered when the session ends and taken up again when the
      backfill names it (`Client::OnVehicleSpawn`). The claim on it waits
      1.5 s after the welcome (`REJOIN_WAIT_MS`), long enough for the seats
      behind the spawns to arrive: if one says somebody else has been
      driving it since, or our old connection still is, the row goes back to
      being their car and ours is claimed as a new one instead of being taken
      off them. A car the backfill never hands back is claimed as new once
      the wait is over. Not run in game.
- [ ] A server that can be run by a person who is not us: config file, clear
      logs, a README that covers port forwarding. The config file is
      `CoopIII-Server.ini`, written with a comment on every setting. The logs:
      on start both the window and `--nogui` say which of the machine's
      addresses to hand out, whether it needs UDP forwarded on the router or
      has a public address of its own, and that the firewall has to let the
      port in (`server/core/reach.h`); a player turned away is told apart by
      name and why, with both protocol numbers for a version mismatch; and
      the three lines that went to stdout past the window's log go through it
      now. A `password` (off by default) keeps strangers out of a server on
      the open internet, and a refused player now hears why and stops asking
      every two seconds (protocol.md §1.27). The README section is what is
      left.
- [x] Desync diagnostics. When someone says "he was in a different place on
      my screen", there has to be something to look at. The F9 list marks a
      player nothing has arrived from for three seconds as "quiet" and for
      how long - the menu, a loading screen, a cutscene or a game left in the
      background, which is what a frozen ped on screen usually is - and its
      last line says how many packets have gone each way and how many peds
      and cars this machine hosts and holds. And the two machines are
      compared: every two seconds a client tells the server where its engine
      has each remote player and each session car it watches, at which
      instant of the owner's clock, and the server answers with the distance
      from where the owner said it was at that instant (`C_DesyncProbe`,
      protocol.md §1.26). Traffic and pedestrian replicas are compared
      with what their host streamed, the same way. F9 shows "off 7.4 m"
      beside a player a metre or more out and names the furthest copy of
      anything else; both logs name the worst past 5 m. Not a version. Not
      run in game.

---

## 4. Sync inventory

Everything that has to travel, and where it stands. Sources are re3 members
(`protocol.md` §1.7).

### Player

| What | Fields | Status |
|---|---|---|
| Transform | `m_matrix` position, `m_fRotationCur` | ✅ sent |
| Velocity | `m_vecMoveSpeed` | ✅ sent |
| Vitals | `m_fHealth`, `m_fArmour` | ✅ sent, and on the join packet as well as the snapshot, so a late joiner creates a ped on the health it really has (`protocol.md` §2.8.1) |
| State | `m_nPedState`, `m_nMoveState` | ✅ sent; `m_nMoveState` is also applied, so the engine picks the walk/run animation itself |
| Animation | `AnimationId` + time, base **and** partial | ✅ sent and applied via `CAnimManager::BlendAnimation`. Run in game 2026-09-22, every weapon the owner tried |
| Weapon | `m_weapons[]`, `m_currentWeapon` | ✅ sent and applied via `CPed::GiveWeapon` + `SetCurrentWeapon`, so the model is in the hand. Ammo is the next row down: on the wire since version 18, behind a server switch |
| Aim | yaw/pitch | ✅ both sent and applied, not yet run in game. Yaw goes in through `CPed::SetAimFlag`; pitch through a detour on `CPedIK::PointGunInDirection` that swaps it in for the 0 `CPed::AimGun` passes every non-player ped, so the engine's own IK bends the arm (pistol, uzi) or the torso (shotgun, AK, M16). The sender reads both angles off that same call, which also fixes the yaw of a player locked on to a target (`m_fLookDirection` is 999999 then). Sniper, rocket launcher, flamethrower and drive-bys never aim through the IK on anyone's screen and stay level |
| Shots | event | ✅ sent reliably and replayed through the real `CWeapon::Fire`, so impacts happen for real (`combat.cpp`). Since 2026-09-22 the shot's own direction is on the wire and the observer aims with it, so the trail, the decal and the line of sight follow the shooter instead of an interpolated ped's heading (`protocol.md` §1.9.7). A sniper round, which `CWeapon::Fire` cannot replay on anyone but the shooter, carries the line the shooter's camera looked down, and the observer plays the report and the impact where that line meets something (`combat.h`, `SniperProbe`); not yet run in game |
| Ammo | `m_weapons[].m_nAmmoTotal` | ✅ since version 18, behind the server switch `SESSION_AMMO_SYNC` (`-ammosync`, off by default). `PlayerStateBody` carries the clip and the total, so with it on an observer sees somebody run dry and reload. With it off a remote player's gun still holds CoopIII's invented 1000 rounds, which is the point of the switch: two players may reasonably not want to share an inventory |
| Damage / death | event | ✅ sent by the shooter, applied by the victim through `CPed::InflictDamage`, with the one exemption that lets an authorised hit past the remote-attacker rule. Run end to end in game 2026-09-21. Death is also **held by the session** and replayed on the join packet, so somebody who joins while a player is lying in the road gets a corpse rather than a live player on zero health (`protocol.md` §2.8.1) |
| Enter/exit vehicle | event | ✅ the remote ped opens the door and climbs in through `CPed::SetEnterCar`, and climbs out through `CPed::SetExitCar`. `UpdateRemoteSeats` drives it on a deadline with `WarpPedIntoCar` behind it, so the seat is guaranteed even when the animation is not (§1.14). An entry is now announced when it starts and says which door it goes in through, so the observer plays the same entry at the same time instead of a teleport to the driver's door (§1.14.7); an abandoned one shuts the door behind it (§1.14.8). Carjacking still plays the ordinary entry rather than the jack animation, and re-examining that after the protocol 22 handover did not change it: `CPed::SetCarJack` bails on a `MISSION_VEHICLE` for any non-player ped, which every replica is on every car a session has (§1.14.6). Not yet run in the game |
| Riding as a passenger | event | ✅ CoopIII's own control, since the original game has none: a key asks for the first free passenger seat and the engine picks it (`game/seat.cpp`) |
| Wanted level | `CPlayerPed::m_pWanted` (**not** `CPlayerInfo`, +0x53C on the ped) | ✅ 4 spare bits of `PlayerStateBody::flags` — three for the level, one for whether it is the player's own or the session's — plus 2 bits of `S_Welcome::flags` for the rule. Per-player by default with GTA Online's shared-vehicle rule; `shared` and `off` are server options. The police themselves travel as ambient replicas and cost nothing extra (**[wanted.md](wanted.md)**). Not yet run in the game |

### Vehicle

Offsets verified 2026-09-21 (Area E) and now in `client/src/game/addresses.h`
with the proof for each, so a "❌ nothing sends it" below is about the wire and
nothing else. The address is no longer the obstacle.

| What | Fields | Status |
|---|---|---|
| Transform | full rotation, not yaw | ✅ sent and applied, as a quaternion (`client/src/quat.h`) |
| Velocities | `m_vecMoveSpeed` `0x78`, `m_vecTurnSpeed` `0x84` | ✅ sent and applied |
| Controls | `m_fSteerAngle` `0x1E8`, `m_fGasPedal` `0x1EC`, `m_fBrakePedal` `0x1F0`, `m_nCurrentGear` `0x204` | ✅ sent and applied |
| Health / state | `m_fHealth` `0x200` (1000 = full), `bEngineOn` `0x1F5` bit 4, `m_bSirenOrAlarm` `0x22E` | ✅ sent and applied. The engine and siren flags are written on change only, the horn (`VEH_HORN`, protocol 31) and the headlights every frame, because the engine takes both back from a parked car by itself (`game/siren.h`, `VehicleFlagsWrittenOnChange`). Rewriting the siren does not restart it; the old reason given here was wrong |
| Appearance | `m_currentColour1/2` `0x19C`/`0x19D` | ✅ carried by the spawn packet, so both machines get the same car rather than two random paint jobs |
| Occupants | `pDriver` `0x1A4`, `pPassengers[8]` `0x1A8`, `m_nNumMaxPassengers` `0x1CC` | ✅ every seat is sent, seated (`SeatRemotePed`) and, since `protocol.md` §2.8.2, remembered by the session so a late joiner is told about passengers and not just drivers |
| Destroyed | `VEH_WRECKED` on the wire | ✅ all four kinds of car have somebody entitled to report it — a driver through `C_VehicleBlowUp`, and the three ownerless kinds (a map generator's car, a traffic car, a session car somebody parked) through `C_UnownedBlowUp`. §5.8, closed 2026-09-22. A traffic car carries the transform it blew up at, since its host stops streaming it the moment it burns out — protocol 16 |
| Extra components | `m_aExtras[2]` `0x19E` | ✅ carried by the spawn packet and applied through `ms_compsToUse` around the constructor, since they are cloned into the clump at construction and cannot be written afterwards. §5.9 |
| Spawn / despawn | `CREATE_CAR` path, `sizeof(CAutomobile)` `0x5A8` | ✅ run in the game, both deletion gates shut |
| Damage model | `CDamageManager` at `+0x288`: panels, doors, lights, wheels | ⚠️ designed, built, and **not run in the game**. [docs/cardamage.md](cardamage.md). Only panels and doors travel, and the other two rows of that list came off the wire with a measurement rather than a shrug: a tyre never bursts in retail 1.0 (`CAutomobile::BurstTyre` is in the vtable and nothing dispatches to it), and a broken light is exactly a damaged panel, so the receiver derives it. Gunfire and explosions turn out not to dent a car at all — they reach `CVehicle::InflictDamage`, which takes health and nothing else — so the one thing that diverges is a collision, which is the one thing nobody simulates twice. A change-only reliable packet, merged as a componentwise maximum because every ladder in `CDamageManager` climbs and none descends. On the wire since version 18 (opcodes 0x3A/0x3B). A car with a driver reports its own dents, and since 2026-09-23 so does a car its custodian is settling (`Client::SendCustodyVehicleDamage`, accepted by `Session::MayReportVehicle`) and a traffic car its host is simulating (`Session::NoteCarDamage`, `cardamage.md` §4). A parked generator car and a session car that has finished settling are simulated by nobody, so each machine keeps its own (§5.8) |

### World

| What | Status |
|---|---|
| Clock | ✅ follows the host's `CClock`, not a clock the server keeps on its own. The host reports at 1 Hz; everyone else is moved only once they are more than 3 game minutes out, so the HUD clock and the sun do not stutter. Run in game 2026-09-21, including a host handover mid-session and the 12-hour jump the engine makes on death |
| Weather | ✅ follows the host's `CWeather`. Both ends of the blend are sent, since a single type describes where the sky is going and not where it is. Needs an in-game run |
| Trains | ✅ the El and the subway run on the server's clock. `CTrain::UpdateTrains` (`0x0054F3A0`) places every train from `CTimer::m_snTimeInMilliseconds` alone, so nothing about a train travels: the client estimates the server's clock from the headers of `S_Welcome` and `S_WorldState` and hands it to `UpdateTrains` for that one call. No wire change. `addresses.h` has the proof, `client/src/sessiontime.h` the estimate. Not yet run in game |
| Planes | ✅ the three airliners and the three Dodos fly on the same clock as the trains. `CPlane::UpdatePlanes` (`0x0054BEC0`) places them from `CTimer::m_snTimeInMilliseconds` the way `UpdateTrains` places the trains, and `game/planes.cpp` hands it the session's clock for that one call. The catch is the two mission Cessnas, which fly from a start time the script stamped with `CTimer`'s own value, so both start times are moved with the clock for the call and put back after (`client/src/planetime.h`). No wire change. Not yet run in game |
| Traffic lights | ✅ every junction on the same clock as the trains. `LightForCars1`, `LightForCars2` and `LightForPeds` (`0x00455760`, `0x00455790`, `0x004557D0`) are `CTimer & 3FFFh` against fixed thresholds and are the only way anything asks a light, so `game/lights.cpp` detours the three and answers from the session's clock, read once a frame, without touching `CTimer`; with no session it calls the engine's own. The one decision a light makes that outlasts a frame is a pedestrian setting off to cross (`CPed::Wait`, `0x004D5DE6`). The walk sign's 256 ms blink reads `CTimer` directly and stays local. No wire change. Not yet run in game |
| Lift bridge | ✅ the Shoreside lift bridge on the same clock. `CBridge::Update` (`0x00413AC0`) works from `(t - epoch) & FFFFh` with an epoch stamped from the local `CTimer`, so `game/liftbridge.cpp` writes the epoch that makes the phase `session & FFFFh` on every machine - agreed without a packet - and re-applies `SetLinksBridgeLights` when a jump onto that clock skips one of the two state changes that switch the traffic links. Only matters after A Drop in the Ocean (`COMMERCIAL_PASSED`); before that the bridge is up and locked everywhere. No wire change. Not yet run in game |
| Pickups | ✅ exclusive. Claim on approach, first claim wins at the server, and the winner's own engine does the awarding through its own `CPickup::Update` switch. Observers replay the engine's removal and push the collection into their own `aPickUpsCollected`, which is what makes a rampage start and a hidden package count everywhere for free. [docs/pickups.md](pickups.md). Not yet run in game |
| Pickups a dead ped drops | ✅ the host reads back what its own engine created - the money roll, the scatter, the ammo - and every observer replays it through `CPickups::GenerateNewOne`, after which it is an ordinary pickup. Fixed a bug on the way: a player's death used to drop a gun on the pavement of every machine except their own |
| Rampage sharing | ✅ §5.10, and this line used to say the opposite. The *start* was already shared and always had been - the pickup work pushes a remote collection into every machine's own `aPickUpsCollected` and every machine's own `rampage.sc` calls `CDarkel::StartFrenzy` off it. What was missing was the counting, and not in the way anybody expected: a pedestrian hosted by one machine and shot from another was credited to **nobody**, because `CPed::InflictDamage` only reaches the kill register when the damager is `FindPlayerPed()` or `FindPlayerVehicle()` (`0x004EAD1A`). Kills now travel as the engine's own three arguments and the ending is arbitrated. [rampage.md](rampage.md). Not yet run in game |
| Vehicle rampages | ✅ since version 29, `C_RampageCar` / `S_RampageCar` (0x8E/0x8F). `CDarkel::RegisterCarBlownUpByPlayer` (`0x00421070`) is **not** the same shape as the ped register: `CAutomobile::BlowUpCar` calls it with no culprit test, so every machine holding a copy of a wreck would count it. The machine that decided the wreck reports it, every replay keeps the register out, and the relay does the counting. `rampage.md` §9. Not yet run in game |
| Pickup mines | ❌ deliberately left local. Script-only, barely used in retail III, and their branch is about arming and exploding rather than giving anybody anything |
| Garages / doors | ✅ all 32 garages. One bit each on the wire - "my own state machine has this one away from where this type rests" - and the union of everybody's bits is what each machine holds its own doors to, so a safehouse opens for whoever walks up and a spray shop closes over whoever is inside. The door's *height* never travels: every machine derives it from the state through the engine's own ramp, which is §1.11's health-versus-destruction argument applied to a door. `protocol.md` §1.16, on the wire since version 18, not yet run in game. **The safehouse pedestrian door and the save point are not garages** and are not covered - they are `main.scm` objects swung by `034D ROTATE_OBJECT`, so they belong to M5 (§1.16.7) |
| Pay'n'Spray | ✅ repair and repaint travel as one reliable event carrying the two colours the owner's engine chose. They have to: the retail `ChooseVehicleColour` is **not** an RNG roll, it is `(m_lastColorVariation + 1) % m_numColours` plus a tiebreak against the local player's own car - both machine-local, so two engines calling it paint two different cars. Same class as §5.9, third time. The wanted level deliberately does **not** travel and there is a marked seam where it would (§1.16.6); an observer is stopped from running the arm that would clear its own player's stars |
| Fires | ✅ §5.7, all three phases. Whatever lit it - a molotov, a rocket, a burning car, a burning player - and it burns the other player for real |
| Explosions | ✅ replayed at the position the owner sends, and the engine's own `CWorld::TriggerExplosion` then damages everything in the radius on every machine |
| Destroyed objects | 🟡 §5.13 and [docs/objects.md](objects.md). Breaking is shared: the machine that owns whatever hit it reports, the host reports what nobody owns, and every observer replays the engine's own `CObject::ObjectDamage`. **Uprooting is shared too**, as one resting place per knocked-over object, sent when the engine's own sleep test says it stopped - because where it lands is local physics (§2.4) and no impulse on the wire would reproduce it. Four of the five measurements took work away rather than adding it: an explosion breaks *and* uproots the same objects everywhere for free, a bullet has never broken one, a bullet cannot uproot a lamp post at all (`object.dat` says 400, the gate is `<= 0`), and the uproot a bullet *can* cause travels with §1.9.2's shot replay. Not yet run in game |
| Ambient peds | ✅ shared, and the crowd does not double - measured in game 2026-09-22 (`population.md` §1.3.1). Since 2026-09-23 they also fight every player, not only the one whose machine hosts them: the host forwards a hit its ped landed on another player's copy to that player, the weapon rides the ped row, and the rounds are drawn on the replica (`population.md` §6). Not yet run in game |
| Ambient traffic | ✅ shared the same way, model, both colours and both extras on the spawn so nobody builds a differently-painted car. Measured the same day, same result: the two engines count the union rather than twice it |

### Campaign

| What | Status |
|---|---|
| Script execution | ❌ host-only, designed not built |
| Mission entities | ❌ |
| Subtitles, blips, spheres | ❌ new packets needed |
| Cutscenes | ❌ |
| Mission pass/fail | ❌ |
| Triggers (Tier 3) | ❌ 3 opcodes, 123 sites |
| Save / progress | ❌ host-owned |

---

## 5. Decisions - settled

These change how the game *feels*, so they were decided deliberately rather
than left to fall out of the code. Decided 2026-09-21. Treat them as settled;
reopen only with the owner.

### 5.1 Wanted level - per-player, GTA Online style, server-configurable

Each player carries their own stars. The police pursue whoever is wanted, not
the group. Propagation follows GTA Online's rules, and the rule that matters
most is that sharing a vehicle with a wanted player shares the heat.

Server option `WantedLevel`:

| value | meaning |
|---|---|
| `perplayer` | **default.** Own stars; shared inside a shared vehicle. |
| `shared` | The whole session shares the highest wanted level. |
| `off` | No wanted level at all. |

**Built 2026-09-22. [docs/wanted.md](wanted.md) is the investigation, the
design and the measurements; it supersedes the paragraph that used to be here
and the table above still stands.** The two things this section had wrong:

- **The name.** The wanted level does not live in `CPlayerInfo`. It hangs off
  `CPlayerPed` at `+0x53C`, which is the end of `CPed` — so the constraint in
  §2.3 is right for a sharper reason than it gave: a `CCivilianPed` is
  *exactly* `sizeof(CPed)` bytes, and every remote player and every replica in
  CoopIII is one, so there is physically nowhere for a second wanted level and
  no engine code that would read one.
- **The cost, which was backwards.** `perplayer` is close to free and `shared`
  is the work. Each machine already runs its own `CWanted`, generates its own
  police from it, and attributes only its own player's crimes to it — the
  engine's `CEventList::RegisterEvent` reports a crime only when the criminal
  is `FindPlayerPed()`, so a replayed shot from another player cannot move
  your stars. And the police need nothing at all: a cop ped is a `RANDOM_CHAR`
  and a police car a `RANDOM_VEHICLE`, so [population.md](population.md) has
  been replicating both since it shipped. What is actually hard is `shared`,
  where a level held by more than one machine needs a rule for coming back
  down that no single machine can decide alone (`wanted.md` §4.5).

The police AI is **not** driven from CoopIII's state, and must not be:
`CCopPed` reaches for `FindPlayerPed()` nineteen times and has no "chase this
ped" path at all. CoopIII writes one number into the engine's own `CWanted`
and the engine does the rest.

### 5.2 Friendly fire - server-configurable, off by default

`FriendlyFire = false`. Groups that want it can turn it on.

### 5.3 Distant players - blip only, not rendered

Beyond the streaming radius a player is a map blip and nothing else: no ped, no
model request, no collision. It is the option that does not fight §2.1: no
tether, no multi-focus streaming, nothing thrashing the streamer's memory
budget.

The consequence to get right is the transition. A player crossing into your
radius must spawn smoothly rather than popping in mid-stride, and one leaving
must despawn without leaving a corpse behind. The handoff is where the work is.

**The marker half of this is done** (`client/src/game/radar.h`), and it is no
longer the part that needs planning.

It was a `BLIP_CHAR` in the game's own `CRadar::ms_RadarTrace` at first, which
made this section a list of things to arrange: a coord blip for players with no
ped, ownership without an entity handle to recognise it by, writing
`m_vec2DPos` by hand every frame because no engine setter moves a coord blip.
None of that is needed now. A remote player is drawn directly, as the rotating
arrow the engine draws the local player with, out of a detour on
`CRadar::DrawBlips` — and an arrow is a position and a heading, both of which
arrive on the wire whether or not there is a ped to hang them on.

So the streaming transition costs one branch, and it is already written:
`ResolveArrow` reads the live `CPed` (or the car it is sitting in) when there
is one and the last snapshot when there is not. `CRadar::LimitRadarPoint` pins
anyone past the radar's range to the rim and `CRadar::CalculateBlipAlpha` dims
them as they go, both the engine's own, so a player walking out of the
streaming radius loses their ped and their nametag and keeps their arrow,
without the arrow changing in any way as they cross.

What is left of §5.3 is the ped half: when to stop spawning, how to despawn
without leaving a corpse, and how to bring somebody back in mid-stride.

### 5.4 Mission failure on death - it fails, as in single player

If a player dies during a mission, the mission fails. Faithful to SP.

This reverses the provisional default in `campaign.md` §2.3, which assumed
co-op should keep going while someone is alive. `campaign.md` §2.3 must be
updated. The server option can still exist, but its default now matches SP.

### 5.5 Fidelity wins by default

Stated once so it stops being case-by-case drift: when single-player behaviour
and co-op convenience conflict, single-player behaviour wins. Server options
may relax it; the defaults do not.

§5.4 is that policy applied.

### 5.6 CoopIII only runs when launched through the launcher

New requirement, and a good one. Dropping `CoopIII.asi` into the game folder
must not change single-player. The mod activates only when the game was
started by `coopiii-launcher`. Started any other way, the `.asi` loads, logs
that it is standing down, and installs nothing.

The player keeps one install for both: the game launched normally for single
player, launched through the launcher for co-op. No files to move, and no
chance of the mod interfering with a solo campaign run.

Implementation: the launcher sets a marker in the child process's environment,
and the boot thread checks for it before doing anything. Environment blocks are
inherited by `CreateProcess` children, so this needs no IPC and cannot be
triggered accidentally.

Status: built. The launcher puts `COOPIII_LAUNCHED` in the child's
environment (`launcher/include/launcher/core.h`, `ENV_LAUNCHED`) and the boot
thread in `client/src/dllmain.cpp` stands down without it, having hooked
nothing, and says so in `CoopIII.log`.

---

### 5.7 Fire is world state, and it gets synced. Three phases.

Decided 2026-09-21, after a live run where a remote player held a flamethrower
and nothing at all came out of it on the other screen.

**Phase one, done: the flame has to appear.** A remote player pulling the
trigger on a flamethrower produces visible fire on every observer, and no
observer decides who burns.

The constraint that had the flamethrower on the refused list is real and has
not been waved away. `CWeapon::FireAreaEffect` hands the shot to `CShotInfo`,
whose slot lives for the weapon's `m_fLifespan` and keeps setting things
alight every frame until it expires, long after the call that created it
returned. A guard wrapped around the call never covered that.

What changed is that the guard is no longer the call. `CPed::InflictDamage`
now refuses anything a remote player's ped tries to take off the local
player's health, which holds for the whole life of the `CShotInfo` and every
`CFire` it lights, because `CFire::ProcessFire` passes its `m_pSource`
straight into `InflictDamage` and that source is the remote ped. No timer, no
engine flag held across frames. `CShotInfo::Update` itself only lights fires
and skips `bFireProof` peds, which every remote player already is.

The exception, and it is the same one §1.9.2 already made: an explosion may
still hurt the local player, because a blast is replayed at a fixed world
position everyone agrees on, so "was I standing in it" is a question about us
that we are entitled to answer.

**Phase two, done: fire burns, whatever lit it, and nobody decides it for
anybody else.**

The two questions phase two was gated on are both answered, against the
retail binary. They are recorded in full in `client/src/game/addresses.h`
under `---- fire ----`; the short version and what each one settled:

**1. The array is fixed, and it is 40 entries of 48 bytes.** Three separate
functions carry the bound and all three agree — `CFireManager::Update`
(`cmp ebp,28h` at `0x00479336`), `GetNextFreeFire` (`cmp eax,28h` at
`0x00479304`) and `FindFurthestFire_NeverMindFireMen` (`cmp ebx,28h` at
`0x004794C4`) — and all three step by `30h`. `gFireManager` is at
`0x008F31D0`, `m_nTotalFires` at `+0`, `m_aFires` at `+4`, so the whole thing
ends at `0x008F3954`. re3's `NUM_FIRES` also says 40; that is a coincidence
that was checked rather than a constant that was trusted, and given that the
last three bugs in this project were all an re3 number that did not match
retail, it is worth saying which of the two this is.

**2. Yes, a `CFire` can point at an entity — and the interesting part is what
it does with it.** `CFire::ProcessFire` (`0x004798D0`) opens on
`mov eax,[ebx+10h]`, and when `m_pEntity` is set it **rewrites `m_vecPos` from
that entity's matrix every single frame** before doing anything else. So an
entity fire's position is not state at all, it is derived; its life is tied to
the entity; the engine keeps a two-way link (`CPed::m_pFire` at `+0x4B4`,
`CVehicle::m_pCarFire` at `+0x1E4`) and asserts it every frame, extinguishing
a fire whose entity no longer points back; and it cannot exist on a machine
where that entity does not.

That answers "snapshot or event stream" with **neither, and the split is by
kind rather than by mechanism**:

- **A fire on the pavement** (`m_pEntity == nil`) is flat, ownerless state at
  a fixed position. It turns out to need nothing at all, which is the other
  finding: `CFireManager::StartFire(pos, size, propagation)` (`0x00479500`)
  has **exactly one caller in the whole image**, at `0x0055957E` inside
  `CExplosion::AddExplosion`. In retail 1.0, an explosion is the only thing
  that puts an unowned fire on the ground, and CoopIII already replays every
  player's explosion at a fixed world position every machine agrees on
  (`protocol.md` §1.9.3). Pavement fires are therefore already the same on
  every machine, for free, and a fire packet would have doubled them.
- **A fire on an entity** is a property of an entity that is already synced,
  so it belongs on that entity's own stream, not in a fire table snapshot.
  That is phase three.

And `CReplay`'s two `memcpy`s are not the precedent they looked like. Replay
is one process, so `m_pEntity` and `m_pSource` still point at something on the
way back in. Across the wire they are per-process pool pointers (§1.5), and
half the array is them.

**So who decides that a fire burned somebody? The victim, always.**
`protocol.md` §1.10.6 is the writeup. Fire damage never goes on the wire in
either direction — not because it was hard, but because a fire's authority is
a *place* rather than a *ray*, and "am I standing in it" is a question about
me that I answer from my own position this frame with nothing stale in it.
Most fires have no owner to send it from anyway, and `CFire::ProcessFire` hits
once per frame, so forwarding it would be sixty packets a second per burning
player.

The one predicate that had the flamethrower's fire refused now admits exactly
one cause, `WEAPONTYPE_FLAMETHROWER`, and it is narrow because the binary
makes it narrow: of the 21 `call CPed::InflictDamage` sites in the image,
exactly two push `9` and both are inside `CFire::ProcessFire`. That cause
cannot mean "a remote player shot me"; it can only mean "a fire is burning me
and it remembers who lit it". Nothing interpolated goes into the decision.

Friendly fire is decided by the fire's own `m_pSource`: null is terrain and
burns anyone, a remote player's ped is their fire and is gated like their
bullets. The molotov puddle is still terrain — `StartFire(pos, ...)` nils the
source — so "it burns whoever walks into it, friendly fire or not" still
holds for exactly the fire that sentence was written about.

No wire change. `PROTOCOL_VERSION` is untouched.

**Phase three, done: a burning player is visible to everyone.**

`PF_ON_FIRE`, one spare bit in the flags byte the snapshot was already
sending. No packet, no layout change, `PROTOCOL_VERSION` still **8**.
`docs/protocol.md` §1.10.7 is the writeup.

**The argument this phase was deferred for turned out not to exist, and the
engine is what says so.** Phase two's objection was real as far as it went:
for a ped that is not the local player, `CFireManager::StartFire`'s entity arm
calls `SetFlee`, `SetMoveState(PEDMOVE_SPRINT)`, `SetMoveAnim()` and
`SetPedState(PED_ON_FIRE)`, straight into the stream `ApplyRemotePose`
overwrites every frame. What phase two did not check is that all four sit
inside one branch, and the engine skips it:

```
00479640  mov [ebp+4B4h], esi     ped->m_pFire = fire
00479646  call 004A1150           FindPlayerPed()
0047964B  cmp ebp, eax
0047964F  je  00479890            -> jumps to 004796C7, past the whole AI
```

So GTA III already has a way to set a ped alight with no burning-ped AI
attached, and it uses it for the one ped whose movement is not the engine's to
decide. A remote player is that ped on this machine. CoopIII takes the same
branch: `LightRemoteFire` in `client/src/game/ped.cpp` is StartFire's shared
tail transcribed in its own order, with the branch not taken — and nothing
else. The engine's AI is never started, so there is nothing to suppress,
nothing to unwind, and no state for the two to fight over.

The other half is that a fire, once lit, writes nothing to the ped at all.
`CFire::ProcessFire` reads the ped's matrix, asserts the two-way link and
calls `InflictDamage`. It never touches `m_nPedState`, `m_nMoveState` or the
clump. **The AI was the whole of the risk and it was all in one branch.**

**Nobody decides anybody's health.** The damage stayed exactly where phase two
put it. An observer's fire on a remote ped is refused twice: `bFireProof` is
the first and only thing `InflictDamage`'s cause-9 arm tests (`0x004EA898`),
and the detour refuses anything aimed at another player's ped before it asks
why. What the fire *is* allowed to do is spread to the local player, which is
single-player behaviour and is §1.10.6's rule already — the local machine
answering a question about itself from a fire in its own street.

**Why an observer saw nothing before, precisely.** Not a missing feature, a
working guard. `CWorld::SetPedsOnFire` tests `bFireProof` before lighting any
ped (`0x004B3D9A`), and every remote player is `bFireProof`, so the replayed
rocket that lights the victim on the victim's machine is refused on every
other. Which is why the flame has to be replicated deliberately or not at all.

**Two rules the reconciliation carries that are not tidiness:**

- **A remote ped's fire is ours or it is wrong.** `CShotInfo::Update` does not
  check `bFireProof` (`0x0055C1A8` gates on `IsPedInControl` and a distance),
  so a replayed flamethrower can still light a remote ped locally — with its
  own `SetFlee` before the call. A fire CoopIII did not light gets
  extinguished, which is also how the engine's own state gets unwound:
  `CFire::Extinguish` calls `CPed::RestorePreviousState`, which pops what
  `SetFlee` stored. CoopIII restores nothing by hand.
- **An observer's fire may not sit on a ped in a car.** `ProcessFire`'s ped
  arm writes `75.0f` into a burning ped's vehicle's `m_fHealth`
  (`0x00479959`) — that is how catching fire wrecks the car you get into, and
  on an observer it would be this machine deciding the health of somebody
  else's car. A burning player who gets in a car stops burning here.

`CPed::IsPedInControl` (`0x004CE6C0`) is the gate for both starting and
keeping, which is what makes this a loop like `UpdateRemoteSeats` rather than
an event handler. `tools/ghost -burn` claims to be alight for four seconds out
of every eight, which is the only way to exercise it without two games and a
rocket launcher.

Script fires (`CFireManager::StartScriptFire`, `0x00479E60`) are the other
loose end and they belong to Area D: only the host runs the script, so a
script fire exists on one machine today. It is the one fire kind with an owner
and no explosion behind it.

### 5.8 A car nobody is driving has nobody to report it. Named work.

**Closed 2026-09-22**, and not quite in the shape this section proposed below:
the three kinds of car nobody drives - a map generator's, a traffic car, a
session car somebody parked - each report their wreck on
`C_UnownedBlowUp` / `S_UnownedBlowUp` (0x38/0x39) under a key the map or the
session already agreed on, and a joiner is handed the map cars that are
already wrecks (`protocol.md` §1.11, §1.20.4; the Vehicle table's "Destroyed"
row). A wrecked session car is still left out of the backfill, as §2.8.5 of
`protocol.md` says. The text
from here on is the section as it was written.

Found while making the join path late-joiner safe (`protocol.md` §2.8) and
**not fixed**, because the half that matters is somebody else's file.

`C_VEHICLE_STATE` is sent by one machine and one only: the driver's. So a
synced car that is parked receives no updates at all, and its row in the
session freezes at whatever its last driver said. Everything the backfill now
carries about a car's condition — health, engine, siren, `VEH_WRECKED` — can
therefore only ever arrive **from inside it**.

Which leaves the commonest way a car is destroyed with no carrier: you blow up
a parked one. Nobody is in it, nobody reports it, the session keeps a healthy
row for it forever, and every joiner from then on is handed a pristine car
standing where a burnt-out shell is on every other screen. That is very
probably the exact path the owner's car took.

It is deliberately open rather than half-built, for two reasons:

1. **The detection does not exist yet.** Whether the engine can be asked "is
   this car destroyed" — and what it actually does when `m_fHealth` hits zero,
   since writing the field does not destroy the car — is live work in
   `client/src/game/vehicle.*`. Building a wire path on top of a detector
   nobody has written would be a protocol bump that carries nothing.
2. **The authority question has an answer already and it should be reused.**
   An ownerless world entity is the host's, exactly as the clock and the sky
   became the host's in §2.7. The host reports the destruction of a synced car
   that the session records no driver for; `Session::DestroyVehicle` is
   already the single place that lands in, and `MayReportVehicle` is already
   the single gate that would have to make an exception for it. Opcodes 0x60
   to 0x6F are free.

The shape to build, when the detector lands: one reliable
`C_VEHICLE_DESTROYED`/`S_VEHICLE_DESTROYED` pair carrying a `netId`, accepted
from the driver or, for a car with no driver, from the host and nobody else.
Not a new field on the snapshot — a parked car has no snapshot to put it on,
which is the whole problem.

### 5.8.1 Who owns a car, and what changing hands means. Decided 2026-09-22.

§5.8 above says who may *report* a car. This says who may *simulate* one, and
it is the rule the two bugs from the 2026-09-22 session both broke.

**The rule.**

> A session car has exactly one owner at a time: the player in seat 0. The
> owner simulates it and streams `C_VehicleState`; every other machine
> corrects it and writes nothing that makes its own engine act on it.
> Ownership moves only through the reliable, ordered `C_EnterVehicle` /
> `C_ExitVehicle` pair. **Between an exit and the next enter the car has no
> owner, and a car with no owner is simulated by nobody** — every machine
> holds it at the last transform the session gave, with its controls and
> velocities at rest.

Three things follow, and none of them needed a wire change: the server already
implements its half (`Session::NoteEnterVehicle` moves the driver out of the
old car first, `MayReportVehicle` refuses a snapshot from anyone but the
driver, and a claim that names a netId the session already has re-claims it
rather than allocating a second one).

1. **Entering a session car as driver claims it.** Already true, through
   `Client::ObservedVehicleWeAreDriving`, and it matched on the engine's own
   pool reference so it cannot pick the wrong one of two identical parked
   cars.

2. **The claimer keeps a row for the car it claimed.** This was the hole. The
   server broadcasts `S_VehicleSpawn` to everyone *except* the claimer —
   correctly; it is a car from the claimer's own world and they already have
   it — so the claimer was the one machine in the session with no record of
   that car. Step out, walk round, get back in, and nothing recognises it:
   the claim path registers the same physical `CVehicle` under a **second
   netId**, and every observer then holds two cars for it, one following the
   driver and one frozen. In the session log that is vehicle 364 and vehicle
   475, same model 104, same extras 3/-1, no despawn between them.

   The row is marked `RemoteVehicle::ours`, which is the whole difference
   between it and an observed car: it is never spawned (the car is already
   there) and never despawned (a car this engine made is not CoopIII's to
   delete — running `DespawnRemoteVehicle` on it would destroy one of the
   player's own traffic cars, quite possibly with the player in it, because a
   socket closed).

3. **The previous owner stops simulating it, and so does everyone else.** The
   owner's own machine stops sending on exit; that was already true. What was
   not is the other end: every observer went on replaying the last driver's
   snapshot at the car — velocity, steering, throttle, engine — every frame
   for the rest of the session. `RemoteVehicle::last` is frozen at whatever
   snapshot happened to arrive last before its owner stepped out, and
   `C_VehicleState` is unreliable and unordered, so that is not even reliably
   the last one sent.

   **That is what makes a used car impossible to get back into.**
   `CVehicle::CanPedEnterCar` (`0x005522F0`, disassembled into
   `addresses.h`) refuses any car whose `m_vecMoveSpeed` or `m_vecTurnSpeed`
   has a magnitude-squared over `0.04`, and `CPed::SeekCar` (`0x004D3F90`)
   answers that refusal with `CPed::RestorePreviousState` while leaving
   `m_objective` at `ENTER_CAR_AS_DRIVER`. There is no timeout in it: the
   next frame walks the ped back to the same door, and the player walks at
   the car forever.

   So a driverless car gets `WorldBridge::RestRemoteVehicle` instead of the
   controls half of `ApplyRemoteVehicle`. Health, damage and `VEH_WRECKED`
   still flow — a joiner has to be shown the shot-up car somebody parked —
   because those belong to the session whoever is or is not driving. What
   takes a driver to mean anything is the throttle and the velocity.

   Rested every frame it is parked, not once on the transition: the transform
   is pinned after physics every frame, so a car held a hair off the ground
   never lands and the engine would keep adding gravity to
   `m_vecMoveSpeed.z`. One frame of gravity cannot reach `0.04`; twenty can.

**What this does not do.** It does not touch the ambient population's cars
(`population.md` §1.3.1 is explicit that traffic replication promises no
handoff). `CorrectAmbientCarReplica` still only *guards* — it stops correcting
a replica the local player is driving without claiming it, so the session goes
on telling everybody else where its owner thinks it is. Getting into somebody
else's *traffic* car is still not an ownership change, and it should be: that
is the next piece, and it is the same rule applied to a second roster.

### 5.8.2 Both of those are built. Done 2026-09-23.

`protocol.md` §1.20 and §1.21 are the design. What changed against §5.8.1 above
is one sentence of it, and it is the sentence that turned out to be the bug.

> *"a car with no owner is simulated by nobody — every machine holds it at the
> last transform the session gave"*

That is right for a car standing in the street and wrong for one that was still
moving when the session stopped having a driver for it, because the hold is
applied **after** physics, every frame. `CVehicle::CanPedEnterCar` refuses a car
whose `up.z` is *inside* ±0.1 — on its side, which is the pose a rolling car
ends up in — and `CPed::SeekCar` answers that with no timeout. So the car is
pinned on its side on every machine and nobody can ever get in again.

A driverless car now gets a **custodian**: one machine, named by the server on
the `S_ExitVehicle`'s own reliable ordered channel and immediately after it,
that stops correcting the car and lets its own engine finish. It streams
`C_VehicleState` for it, every observer follows, and when the car comes to rest
it says so and the session goes back to nobody simulating it — i.e. back to the
rule above, which is where the bandwidth and the stillness come from. **Custody
is the exception; rest is the rule, and a car with no custodian takes exactly
the path it took before.**

**Granted to the player who was driving, not to the host, and §5.8's answer is
still right for what §5.8 is about.** The host is the right authority for a
*fact* about an ownerless entity — it is one machine and it is always there.
It is the wrong machine to run a car's physics on, because §2.1 and §2.2 above
mean a host across the river has neither the car streamed in nor the collision
under it. Custody is therefore short (2 s) and goes to the machine that was
touching the car a frame ago.

That machine is also whoever drives into the car afterwards. A car nobody holds
used to be a wall on every screen, since every machine pinned it after every
frame of physics; now the machine whose car shoves it asks for it the way a
shot does and settles it for as long as the shove lasts (`VEHICLE_HIT_PUSH`,
protocol.md §1.21.5, 2026-09-24). A shove by somebody else's traffic or by a
player on foot still meets the pin. Not run in game.

Traffic is the same rule applied to the second roster, and the answer to "the
same claim or something narrower" is **the same claim**: an `AmbientCar` has an
owner and no seats, so a player at its wheel is invisible to it and every
observer draws them in the road. The claim is `C_EnterVehicle` with the netId
the session already has (netIds are one space, so it is unambiguous), the row
is promoted **under the same netId**, and no car is created or destroyed on any
machine — including the one whose own engine made it, which keeps its `CVehicle`
and merely stops being allowed to report it.

§5.8's own subject — a *parked* car nobody has ever claimed being blown up —
is still open and is still the host's. Nothing here changes that; what it adds
is that a car being settled has an owner, so its destruction travels on the
ordinary vehicle blast path for the length of the settle.

### 5.9 A car's extra components are picked per machine. Named work.

**Done**: `EnterVehicleBody` and `S_VehicleSpawn` carry `m_aExtras`, forced on
the receiving machine through `CVehicleModelInfo::ms_compsToUse` before the
constructor runs (`protocol.md` §1.12). One thing below turned out wrong: the
extras cannot be written after construction, because they are cloned into the
clump inside it. The rest is the section as it was written.

`CVehicle::SetModelIndex` (`0x00551170`) copies
`CVehicleModelInfo::ms_compsUsed` into `m_aExtras[2]` at construction, and the
model info picks those at random. So every machine that spawns a given car
chooses its own set: one player sees a Mule with a roof rack, another sees the
same Mule without one.

**This is not a late-joiner gap** and that is why it is here rather than in
the join work. Two players who connected together already disagree, because
each of their engines picked independently the moment the car was created. It
is the same class as the paint job, which *is* carried — the colours went onto
the spawn packet for exactly this reason.

Cheap when somebody wants it. `m_aExtras` is two bytes sitting immediately
after `m_currentColour2` in `CVehicle` (the `static_assert` in `addresses.h`
pins all four as consecutive), `S_VehicleSpawn` has room, and
`EnterVehicleBody` already reserves a spare `pad` byte in precisely that slot
next to the colours. Both halves are in `client/src/game/vehicle.*`: the
claimer samples them beside the colours, the receiver writes them after
construction and before the model is set up.

---


---

### 5.10 A rampage is shared - one at a time, for the whole session

Decided 2026-09-22. Collecting a `KILLFRENZY` pickup starts the frenzy for
everybody, everybody's kills count toward it, and passing or failing it does so
for the whole session.

**The engine gives no real choice.** A rampage is `CDarkel`, and `CDarkel` is a
singleton with one kill count, one timer and one HUD counter. Retail's own
`CanBePickedUp` already refuses a second killfrenzy pickup while one is running
(`CDarkel::FrenzyOnGoing`, `0x00420E60`). Per-player rampages would mean
CoopIII owning the frenzy state, the target, the clock and the HUD for every
player - that is not a pickup feature, it is a reimplementation of `CDarkel`,
and it belongs nowhere near M4.

**It is also the better game.** "Kill 20 Diablos in two minutes" is the most
obviously co-operative thing in GTA III. Eight private rampages happening in
the same street is worse in every way.

**And it costs nothing to build**, which is the part worth knowing: every
machine runs `rampage.sc`, and that script asks `HAS_PICKUP_BEEN_COLLECTED`.
Pushing a remote collection into each machine's own `aPickUpsCollected` ring
makes every machine's own script start the same frenzy in the same frame, with
its own HUD, its own timer and its own failure condition. No packet for any of
it.

**The trade-off, plainly:** the difficulty is not rebalanced, so a four-player
rampage is trivial. Accepted for M4. If it matters later the fix is a
server-side multiplier on the kill target - which needs the script intercepted,
i.e. M5 - and **not** a per-player split.

> **Corrected 2026-09-23.** The multiplier does *not* need the script
> intercepted. `CDarkel::StartFrenzy` is `0x004210E0`, it takes the kill target
> as its third argument, and it has exactly two callers in the whole image -
> both of them the script opcodes `01F9` and `0367`. So the multiplier is a
> detour on one function and no script work at all, and it was built in the
> rampage round as server option `rampages = shared | scaled | off`, default
> `shared`, which is this section unchanged. `rampage.md` §4.
>
> The rest of §5.10 survived contact with the binary as written. What it did
> not know was that a co-op NPC kill counted for *nobody* rather than for the
> killer: the credit test at `0x004EAD1A` accepts only `FindPlayerPed()` and
> `FindPlayerVehicle()`, and in a session neither of them is the shooter.

### 5.11 A hidden package collected by one player counts for everybody

Decided 2026-09-22. Server option `hiddenPackages = shared | perplayer`,
default **`shared`**. `perplayer` was named in M4 and built 2026-09-23 on the
server alone (`Session::PickupIsPerPlayer`; `pickups.md` §6): each player's
claim on a package is weighed against their own record, nobody else is told,
and each machine's own count is its own player's. Not run in game.

- **The engine has one counter.** `m_nCollectedPackages` is `CPlayerInfo+0xB4`
  and there is exactly one `CPlayerInfo` (§2.3). Remote players are `CPed`s.
  Per-player packages would be CoopIII's own state, and `rewards.sc` - which
  polls the count to unlock the weapons at the hideout - would then be reading
  a number that means something different on every machine.
- **The array cannot hold the alternative.** 100 packages per player against
  320 general pickup slots is three players, and the object pool pays for every
  one of them.
- **§5.5, fidelity wins.** The single-player experience of hidden packages is
  that the map empties as you clear it. A shared world where seven players walk
  past a package only the eighth can see is the version that feels wrong.
- **Same free mechanism as §5.10.** The observer increments its own counter and
  pushes the collection into its own `aPickUpsCollected`, and its own
  `packages.sc` and `rewards.sc` produce the "34 of 100" message, the reward
  weapons and the million at 100 by themselves.

**The trade-off, plainly:** eight players finish the packages in an eighth of
the time, and the 100% grind - one of the longest solo activities in III -
collapses. That is the right price for a shared map, and groups who disagree
get the option.

### 5.12 Ammunition is reported honestly - server-configurable, off by default

Decided 2026-09-22. Server option `ammoSync = true | false`, default
**`false`**. `docs/protocol.md` §1.9.6 is the wire and the engine argument;
this is the decision.

`false` is what CoopIII has always done: a remote player's gun is handed a
fixed thousand rounds at spawn and nobody ever watches anybody else run dry.
`true` puts each player's real count for their own weapons on the wire, so a
firefight has the same numbers on every screen.

- **It is not a shared inventory, and that is the point.** Two players
  carrying different weapons is the normal case and stays that way. The option
  decides whether *your own* counts are honest on *other people's* screens,
  not what anybody is carrying.
- **Off by default on §5.5 grounds, inverted.** Honest ammunition is the
  higher-fidelity answer, but it is also the one that can be *wrong* - it puts
  a number on the wire that the observer's own engine is simultaneously
  editing, and §1.9.6 is a page about keeping those two from fighting. A
  default that cannot misbehave is worth more than a default that is more
  faithful when it works.
- **Same shape as §5.2.** One bit in `S_Welcome.flags`, enforced by the server
  refusing to relay and by the client refusing to apply.
### 5.13 A broken street object is a latch, and the measurement took most of the job away

Decided 2026-09-22. [docs/objects.md](objects.md) is the investigation; this is
what was settled and why it is smaller than it looked.

**Breaking is a state, not a destroy-and-replace.** `CObject::ObjectDamage`
(`0x004BB240`) frees nothing, allocates nothing and touches no world list -
every one of its nine arms writes flags on the object that is already standing
there. So the whole feature is a one-way latch: two players breaking the same
crate is not a conflict, applying a break twice is a no-op, and there is no
creation, deletion or ownership handshake in it anywhere. That is the reason
this is a much smaller thing than an ambient ped or a car, and it was not
obvious going in - the intuitive guess is that the engine swaps in a broken
object, and it does not.

**Two measurements struck most of the planned work, which is now four times.**

- **An explosion already agrees.** The object arm of
  `CWorld::TriggerExplosionSectorList` computes its damage as
  `300 * min((radius - distance) * 2 / radius, 1)` - two positions and a
  radius, no RNG, no impulse, no timestep - and CoopIII already replays every
  explosion at a position every machine agrees on. Objects blown up by a blast
  therefore break identically everywhere for free. This is §5.7's pavement-fire
  finding again, reached the same way. The seam goes deliberately quiet inside
  `CWorld::TriggerExplosion` (two callers in the whole image) rather than
  sending one reliable packet per bin per rocket.
- **A bullet has never broken one.** The object arm shared by
  `CWeapon::DoBulletImpact`, `FireShotgun` and `FireMelee` adds sparks, clears
  `bIsStatic` and applies a force, and stops. A whole-image scan for calls to
  `ObjectDamage` finds five sites and **none of them is in `CWeapon`**. The
  brief for this work said "a car driving into them and gunfire"; the binary
  says gunfire was never in it.
- **And a bullet cannot uproot one either.** That arm's uproot gate is
  `GetIsStatic() && m_fUprootLimit <= 0.0f` - the three constants it compares
  against all read `00000000`, so it is a sign test and not a threshold - and
  `data/object.dat` gives lamp posts 400, traffic lights 500, barriers 350,
  meters and bins 100, cones 10 and benches 5. Every one is above zero, and
  because the object is still static the move force behind `!GetIsStatic()` is
  skipped too. **Shooting a lamp post in retail 1.0 produces eight sparks and
  a sound and moves nothing.** The only breakable models a bullet knocks loose
  are the zero-limit ones - crates, wooden boxes, wastebins, pallets,
  newspaper machines - and those it knocks loose on every machine, because
  §1.9.2 replays the shot through the engine.

So the entire remaining feature is *somebody drove into it*, and that is what
was built.

**Named by where the map put it**: `m_objectMatrix`'s position plus the model
index, the pickup's answer rather than the parked car's or the ambient ped's.
A map object has no generator index to borrow and needs no server-allocated
name, because the map already placed 1851 identical copies before a packet was
sent. The equivalent of `pickups.md`'s 312-coordinate check was done: all 1851
breakable map instances compared pairwise inside each model, closest pair
0.5992 m, nothing under 0.50 m. `tools/objecttest` carries that number so the
tolerance cannot quietly grow into it.

**§5.8's answer had to move one entity across, and the binary is what says
so.** An ownerless world entity is the host's - except that
`CPopulation::ManagePopulation` turns any map object more than 80 m from the
local player back into a pristine dummy, so an object across town from the host
is not a `CObject` on the host at all and the host has nothing to observe.
§5.8 works for a parked car because the *server* holds a row for it whatever
the distance; here there is no row and there must not be one. What transplants
is the shape: exactly one reporter, chosen by ownership - **the machine that
owns whatever broke it**, and the host for whatever nobody owns.

That same 80 m rule is why there is no server table and no backfill. The engine
throws the state away when the last player leaves the block, so a broken object
has a lifetime of one visit and nothing a joiner could be told would still be
true by the time they finished loading. It is also why divergence is
self-limiting: once everybody walks away, both machines are pristine again.

**The open half, now closed.** Uprooting - a lamp post falling over - really is
a different mechanism from breaking, and `CObject::ObjectDamage` is what proves
it: none of its nine arms clears `bIsStatic` and the smash arm *sets* it. What
makes an object fall over is `bIsStatic` being cleared and the object being
handed to `CPhysical::AddToMovingList`, decided in three places that all read
`m_fUprootLimit` - a collision (`impulse > limit`), a blast (`power > limit`)
and a bullet, a pellet or a bat (`limit <= 0`). A lamp post's limit is 400 and
its break threshold is 150, so the same car bends it at 200 and knocks it down
at 500: two decisions off one number, which is why one travelling never implied
the other.

Two of the three already agreed. A blast's power is the same pure function of
two positions and a radius that its damage is. A bullet's uproot rides §1.9.2's
shot replay - every observer fires the remote ped's own `CWeapon` through the
engine, out of the wire's muzzle and along the wire's direction, so their own
`DoBulletImpact` runs the same object arm - and in any case `object.dat` gives
every lamp post an uproot limit of 400 against a gate of `<= 0`, so **a bullet
cannot knock one over on anybody's screen, including the shooter's**. What
nobody else ran was somebody else's *collision*.

So what travels is the resting place and nothing else: one packet per
uprooting (`C_/S_ObjectSettled`, `0xC2`/`0xC3`), sent when
`CPhysical::ProcessControl`'s own sleep test - ten quiet frames, then
`SetIsStatic(true)` - says the object has stopped. Not an impulse: §2.4 means
two machines handed the identical impulse put the post down in two different
places. The receiver writes the matrix through the engine's own
`CMatrix::UpdateRW`, `CEntity::UpdateRwFrame` and `CPhysical::RemoveAndAdd`,
and setting `bIsStatic` is all the moving list needs, because `CWorld::Process`
unlinks a static entity itself on its next pass. Opcodes `0xC4`-`0xCF` stay
reserved. `docs/objects.md` §8 is the whole argument.

### 5.14 Cheats run where what they change is owned - server-configurable, all on by default

Decided 2026-09-23. Server option `cheats = shared | personal | off`, default
**`shared`**. [docs/cheats.md](cheats.md) is the investigation, the table of
all 23 cheats in retail 1.0 and the argument for each; this is the decision.

- **`shared`**: every cheat works, the way it does in single player (§5.5).
  The thirteen about the player who typed them - weapons, money, health,
  armour, stars, the skin, the tank, the three handling toggles, NASTYLIMBS -
  run on their machine and nowhere else, because what they change already
  travels. The four weather cheats go to the host, whose sky is the session's
  (§2.7). TIMEFLIESWHENYOU, BOOOOORING, MADWEATHER, ITSALLGOINGMAAAD and
  WEAPONSFORALL run on every machine, because each machine's own clock and
  crowd is its own and nobody can apply them for anybody else. BANGBANGBANG
  runs where it was typed; the BlowUpCar detour already refuses every car
  somebody else owns, and the wrecks it is allowed travel.
- **`personal`**: only the thirteen. The ten that change the world are refused
  with a log line that names the setting.
- **`off`**: none while connected.
- **Why a switch at all.** A riot, armed pedestrians or a slowed clock that one
  player types lands on everybody, and that is exactly the kind of thing
  players can disagree about. Same shape as §5.1 and §5.10: two bits in
  `S_Welcome.flags`, enforced by the server refusing to relay and by the
  client refusing to run or apply.
- **What is not routed, on purpose.** BANGBANGBANG could have been sent to
  everybody so every car in the city goes up. It is not: that would blow up
  other players' cars with them inside, which is what friendly fire off
  (§5.2) exists to prevent.

## 6. Rules that keep paying off

Learned the hard way this far in; worth not relearning.

- **Never resolve a rel32 by hand.** Use `tools/calltarget`. Hand arithmetic
  produced two wrong addresses in one sitting; one crashed the game, and it
  crashed *in a completely unrelated place*, minutes later.
- **A guess and a proof must not look alike.** Verified addresses live in
  `addresses.h` with a note on how they were proved; everything else lives in
  `addresses-unverified.md`. Two symbols have already been refuted.
- **Do what the engine does.** The ped spawn path works because it is the
  game's own `CREATE_CHAR` sequence, found by walking the opcode dispatcher,
  not a hand-rolled pool allocation.
- **Fail loudly.** With this many mods patching the same binary, "CoopIII
  loaded and quietly did nothing" is the failure mode to design against.
- **Test what can be tested without the game.** Six suites cover the protocol,
  the roster, interpolation, patterns and hooks. What is left needing a live
  game is then small enough to reason about.
