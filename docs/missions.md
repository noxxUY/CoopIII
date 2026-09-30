# Missions, side jobs and rampages

What the scripted half of GTA III can become in a session, and whether that
needs a Sanny Builder SDK and a modified `main.scm`.

The question, on 2026-09-24: free roam is nearly done, so what can we do with
the rampages, the missions and the rest? Do we write a mission
SDK for Sanny Builder and modify `main.scm`, or is there an easier way?

This file is the investigation. `campaign.md` is still the campaign's design
document. Where the two disagree, the measurement is here and `campaign.md` has
a dated note pointing to it.

**The first of it is built** (§15 says what, and what is next). The rest is
the plan that was settled on, with the numbers behind it. [mission-audit.md](mission-audit.md) checks every
one of the 80 missions against it and adds sixteen requirements this file
doesn't cover. §9 records what was decided on 2026-09-24:
a shared campaign, everybody paid, a death failing the mission, everybody at
the start and at every checkpoint. §10 to §14
work out what those decisions need: difficulty for more players, everybody in
the cutscenes, and the rest of a full playthrough. §5.8 (2026-09-29) makes
the campaign the host's save: a guest sees the host's contacts, not its own
save's, and starts no story mission of its own.

---

## 0. The short answer

1. **Don't ship a modified `main.scm`.** That route is slow: every mission
   gets rewritten by hand. For CoopIII it would also mean putting Rockstar's
   script in the download, breaking every existing save, colliding with any
   mod that replaces `main.scm`, and it would *still* need the whole
   engine-side layer underneath (§4.1).
2. **The easier way is the one CoopIII already uses.** Pickups, garages,
   hidden packages and rampages work because every machine runs its own stock
   `main.scm`, and CoopIII answers the few questions where machines have to
   agree at the engine's own seams. Missions are the same problem at a bigger
   scale. Keep the stock script, run each mission on the machine of the player
   who started it, and replay what it shows on everybody else's machine through
   the engine's own interpreter (§4.2, §5). That's one mechanism for all 80
   missions. The work is per opcode, not per mission, and about a hundred
   opcodes cover what other players need to see.
3. **A Sanny Builder SDK is still worth building, for new content.** Make it
   CLEO scripts (`.cs`) compiled in Sanny Builder that call `COOP_*` opcodes.
   CoopIII registers those through III.CLEO's `CLEO_RegisterOpcode` export,
   and III.CLEO is already on the target install. That means no change to
   `main.scm` and nothing of Rockstar's in the download, and CLEO Redux exposes
   the same commands to JavaScript. It's good for races, co-op rampage
   variants and new missions. If one stock mission ever needs a hand-made co-op
   version, the same toolchain can override just that mission when it launches,
   instead of replacing the whole script (§4.3).
4. **Rampages are done in code and have never been run in a game.** They're
   shared by default, with `scaled` and `off` as options, for pedestrians and
   for cars. Running them in a game is the first thing to do. What's left is in
   §6.

---

## 1. What was looked at

- **The script.** This is Lighnat0r's Sanny Builder decompilation, the one
  `campaign.md` §5 already uses as a map. It's cloned outside the repo and
  never vendored. The numbers below come from `GTA 3 Main Variables
  Named.txt`, which is the whole of `main.scm`: 103,704 lines and all 80
  missions. The compiled `main.scm` beside it supplied the sizes. None of the
  numbers come from the 78 `.sc` files, which convert about fifty missions to
  high-level syntax. `campaign.md` §4's table matches those files exactly, and
  §3.2 shows what they miss. Command names are the ones plugin-sdk's
  `plugin_III/game_III/enums/eScriptCommands.h` gives each id. The survey
  scripts are throwaway, and anybody can reproduce every number here with a
  hundred lines of Python over that file.
- **III.CLEO**, cleolibrary/III.VC.CLEO. It's the source of the
  `III.CLEO.asi` on the target install (`compat.md` §1). It shows what CLEO
  patches in `gta3.exe` and how a plugin adds opcodes.
- **CLEO Redux**, from its docs: GTA III 1.0 support, how it runs next to
  III.CLEO, and its plugin SDK.

Addresses quoted from III.CLEO's source are CLEO's claims about the retail
image, not proofs. The ones this plan depends on are listed in §8, to be proved
the way `addresses.h` proves everything else.

---

## 2. Where things stand

### 2.1 Every machine already runs its own campaign

`protocol.md` §3 and `campaign.md` §4 describe free roam ("Tier 1") as clients
suppressing `CTheScripts::Process()`. That was never built, and the free roam
work since then depends on it not being built:

- `pickup.h`: "CoopIII does not suppress the main script on clients ... so
  every machine runs main.scm and creates all 448 script pickups itself".
- `rampage.md` §1: every machine's own `rampage.sc` starts the frenzy.
- `roadmap.md` §5.11: every machine's own `packages.sc` and `rewards.sc`
  produce the package message and the rewards.
- `protocol.md` §1.16.1: all 32 garages come out of `main.scm` on every
  machine.

So a session today is N single-player campaigns in one shared city. Walk into
a mission marker and your own engine plays the mission, alone:

- mission peds are `MISSION_CHAR`s and mission cars `MISSION_VEHICLE`s. The
  population host tests refuse both on purpose (`IsAmbientPedWeShouldHost` and
  its car twin in `population.cpp`: "a mission ped belongs to the campaign"),
  so nobody else sees them;
- the objective text, the blips and the markers are on one screen;
- the progress goes into that player's own save.

That isn't the bug to fix first. It's the foundation. It's also why "any
player can start a mission", the Tier 3 that `campaign.md` expected to be the
hard part, turns out to be nearly free (§5.2).

### 2.2 Rampages are built

`rampage.md` is the whole story. In short:

- **The start is free.** A collection is pushed into every machine's own
  `aPickUpsCollected`.
- **Kills travel as the engine's own three arguments**, from a detour on
  `CDarkel::RegisterKillByPlayer`.
- **The ending is arbitrated**, with the one caller of `CDarkel::ReadStatus`
  (script opcode `01FA`) holding every machine's `rampage.sc` on the same
  value.
- **Two server options.** `rampages = shared | scaled | off`, and since
  version 29 car rampages count too.

None of it has been run in a game.

---

## 3. The script, measured

### 3.1 What's in it

`main.scm` holds the main script and 80 missions. By kind:

| Missions | What they are | How they start |
|---|---|---|
| 0 | the intro | a new game |
| 1-2 | the hospital and police-station info scenes | the info pickup outside each, in Portland View |
| 3-6 | RC Toyz (the RC car comes back in 75, Toyminator, a story mission) | the TOYZ van at a spot |
| 7-9 | the three 4x4 checkpoint runs | the right vehicle in the right zone |
| 10 | Multistorey Mayhem | a Stallion in the car park |
| 11-14 | Paramedic, Firefighter, Vigilante, Taxi | the vehicle and the sub-mission button |
| 15-18 | Marty's payphone jobs | the phone |
| 19-79 | the story: contacts and payphones | the marker or the phone |

`set_total_missions_to 73` is what the 100% counter counts.

The compiled header gives the sizes. The main section is 107,512 bytes, which
is 82% of the 128 KB it has to fit in (`campaign.md` §1.1). The largest mission
is ASUKA1 (Sayonara Salvatore) at 16,845 bytes of the 32 KB mission slot, and
the median mission is 2,641 bytes. The missions hold 68,742 instructions using
671 distinct opcodes; the main section holds 10,305.

### 3.2 How a mission starts, and what `campaign.md` §4 got wrong about it

One opcode starts every mission in the game:
**`0417 LOAD_AND_LAUNCH_MISSION_INTERNAL`**, at 85 sites, all of them in the
main section. What leads up to it depends on the kind:

| Kind | Trigger | Gate before `0417` |
|---|---|---|
| story contacts | `00F6 LOCATE_PLAYER_ON_FOOT_3D` at the marker (115 sites in main) | `$ONMISSION == 0`, then **`03EE CAN_PLAYER_START_MISSION`** (69 sites in main: 66 launches and the 3 save points), then `MAKE_PLAYER_SAFE_FOR_CUTSCENE`, a fade and the title |
| payphones | `00F9 LOCATE_STOPPED_PLAYER_ON_FOOT_3D` at the phone | the same `03EE` gate |
| odd jobs | `00DE IS_PLAYER_IN_MODEL`, then `00E1 IS_BUTTON_PRESSED` button 19 | `$ONMISSION == 0` only |
| RC, 4x4, Mayhem | the vehicle model inside a zone or area | `$ONMISSION == 0` only |

`campaign.md` §4 says the campaign's triggers are `IS_PLAYER_IN_AREA_3D` (63
sites), `IS_PLAYER_IN_AREA_2D` (57) and `IS_PLAYER_IN_ZONE` (3). It also says
"none of the angled, on-foot or in-car variants appear anywhere in the
campaign". The first two numbers match the 78 `.sc` files exactly, which is
almost certainly where they came from. But those files spell most commands as
raw opcodes, and a count by name misses them.

Over the whole script the counts are: `LOCATE_PLAYER_ON_FOOT_3D` at 126 sites
(these are the mission markers), `LOCATE_PLAYER_IN_CAR_3D` at 101,
`IS_PLAYER_IN_ZONE` at 101 and `IS_PLAYER_IN_ANGLED_AREA_ON_FOOT_3D` at 4. The
markers never use `IS_PLAYER_IN_AREA_*`. So the trigger surface isn't three
opcodes. §5.2 explains why that turns out not to matter.

### 3.3 How much of it is about "the player"

The player's handle, `$PLAYER_CHAR`, is an argument to **97 distinct opcodes
at 3,760 sites**: 2,933 in the missions and 827 in the main section. By use:

- **2,052 sites are conditions**, across 55 opcodes. Ten opcodes cover 70% of
  those sites:

  | Opcode | Sites |
  |---|---|
  | `IS_PLAYER_PLAYING` | 341 |
  | `IS_PLAYER_IN_CAR` | 197 |
  | `IS_PLAYER_IN_MODEL` | 147 |
  | `LOCATE_PLAYER_ON_FOOT_3D` | 126 |
  | `IS_PLAYER_IN_ANY_CAR` | 118 |
  | `LOCATE_PLAYER_ANY_MEANS_2D` | 114 |
  | `LOCATE_PLAYER_ANY_MEANS_CHAR_2D` | 104 |
  | `IS_PLAYER_IN_ZONE` | 101 |
  | `LOCATE_PLAYER_IN_CAR_3D` | 101 |
  | `IS_PLAYER_IN_AREA_2D` | 89 |

- **1,708 sites are actions**, across 42 opcodes. Ten opcodes cover 74% of
  those sites:

  | Opcode | Sites |
  |---|---|
  | `SET_PLAYER_CONTROL` | 213 |
  | `SET_CHAR_OBJ_KILL_PLAYER_ANY_MEANS` | 184 |
  | `SET_CHAR_OBJ_KILL_PLAYER_ON_FOOT` | 167 |
  | `CLEAR_WANTED_LEVEL` | 118 |
  | `SET_POLICE_IGNORE_PLAYER` | 111 |
  | `STORE_CAR_PLAYER_IS_IN` | 107 |
  | `ADD_SCORE` | 95 |
  | `SET_PLAYER_COORDINATES` | 91 |
  | `SET_EVERYONE_IGNORE_PLAYER` | 88 |
  | `SET_PLAYER_HEADING` | 84 |

Two of those deserve a line each. The two KILL_PLAYER objectives, 351 sites
between them, are how a mission tells its enemies who to fight, and it is
always the one player. `ADD_SCORE` is how a mission pays.

### 3.4 What a mission leaves behind

A mission isn't self-contained, and this is the fact that decides how shared
progress has to work (§5.5). It leaves three kinds of trace:

- **Globals.** Missions write 1,933 distinct globals with plain value opcodes.
  A III script has 16 locals, so most of these are working storage
  (`$TAXI_DESTX1`, `$FINISH_X`). A few are the progress the main script waits
  on, such as `$DONT_SPANK_MA_BITCH_UP_COMPLETED` and `$FLAG_STAUNTON_OPEN`.
- **Main-script threads.** The missions contain 86 `004F START_NEW_SCRIPT`,
  and every one targets a label in the main section. Passing a mission is
  usually what starts the next contact's trigger loop.
- **World state the save keeps.** Counted in the missions only:

  | Opcode | Sites |
  |---|---|
  | `SWITCH_CAR_GENERATOR` | 167 |
  | `SET_VISIBILITY_OF_CLOSEST_OBJECT_OF_TYPE` | 37 |
  | `SWAP_NEAREST_BUILDING_MODEL` | 32 |
  | `SWITCH_ROADS_ON/OFF` | 23 |
  | `SET_TARGET_CAR_FOR_MISSION_GARAGE` / `CHANGE_GARAGE_TYPE` | 33 / 6 |
  | `PLAYER_MADE_PROGRESS` | 84 |
  | `REGISTER_MISSION_PASSED` | 73 |
  | `ADD_SCORE` | 89 |

  The two object swaps are the building swaps and invisibility settings that
  the save's script block carries.

Whichever machine runs a mission, all three have to reach every other
machine's own main script. Until they do, the session is N campaigns, not a
shared one.

### 3.5 What other players would have to see

These are the presentation opcodes, whose whole effect is on a screen. Counted
in the missions:

| Group | Distinct opcodes | Sites | Largest |
|---|---|---|---|
| HUD text, timers, counters, pager, help | 40 | 2,941 | `PRINT_NOW` 1,924 |
| camera, fades, widescreen, cutscenes | 26 | 2,894 | `GET_CUTSCENE_TIME` 818 |
| blips, markers, coronas, spheres | 22 | 2,110 | `REMOVE_BLIP` 1,179 |
| mission audio and sounds | 10 | 551 | `ADD_ONE_OFF_SOUND` 121 |

That's about a hundred opcodes and 8,500 sites, one instruction in eight.
There are also 79 cutscenes (`LOAD_CUTSCENE`). The missions create peds at 446
sites (`CREATE_CHAR` 314, `CREATE_CHAR_INSIDE_CAR` 73,
`CREATE_CHAR_AS_PASSENGER` 59), cars at 170, objects at 87 and pickups at 21.

---

## 4. The options

### 4.1 A modified `main.scm` and a Sanny Builder SDK

The route goes like this:

1. **Decompile `main.scm`.**
2. **Add custom opcodes**, `COOP_*` commands such as print, blip and
   checkpoint for one network player, collect the players for the mission,
   is host, locate all players, teleport the players to the host, and enable
   syncing this script.
3. **Edit every mission** to call them.
4. **Compile it with Sanny Builder 4**, through a custom mode that teaches it
   the new commands.
5. **Ship the compiled `main.scm` and `script.img`.**

Underneath that there still has to be a list of opcodes that the host's
script runs and every client replays, with the client's `CollectParameters`
fed the host's values.

What it would cost CoopIII, point by point:

1. **Rockstar's code in the download.** The README says "No Rockstar files or
   assets are included here". A recompiled decompilation of `main.scm` is
   Rockstar's script.
2. **Saves.** A save's script block holds the globals and every running
   script's instruction pointer (re3's `CTheScripts::SaveAllScripts`), and a
   recompiled script moves both. Existing saves stop loading, and a co-op save
   stops loading in single player.
3. **Mods.** `compat.md` commits CoopIII to being the guest, and a mod that
   ships its own `main.scm` is exactly what a CoopIII `main.scm` would
   displace. Every player would also need the byte-identical file.
4. **Room.** The main section is already at 82% of its 128 KB.
5. **The work doesn't go away, it multiplies.** You'd rewrite 80 missions and
   *still* need everything in §4.2 underneath.
6. **Fidelity.** `campaign.md` §5: "A patched campaign script is a different
   campaign."

What it would buy is per-mission control over the co-op design. That's real,
and §4.3 keeps it in a contained form: override one mission, not the script.

**Verdict: not as the base.**

### 4.2 The stock script, with a bridge in `CoopIII.asi` (recommended)

This is `campaign.md`'s design (the host runs the mission, everybody else gets
its effects) with three corrections found since it was written:

1. **Nothing is suppressed on clients.** Every machine keeps running the main
   script, as it already does (§2.1).
2. **A mission runs on the machine of the player who started it**, not on the
   session host (§5.1).
3. **Triggers only need arbitrating, not intercepting** (§5.2).

What it costs:

- about a hundred presentation opcodes replayed;
- a few dozen persistent world opcodes replayed;
- a policy for the twenty or so player opcodes that matter;
- mission entities added to the population roster;
- a campaign delta for shared progress.

What it buys:

- all 80 missions at once;
- saves and mods left alone;
- nothing of Rockstar's shipped;
- the missions played as written.

### 4.3 CLEO scripts with CoopIII opcodes: the SDK, without touching `main.scm`

- **Registration.** III.CLEO exports `CLEO_RegisterOpcode(id, handler)`
  (stdcall, `CLEO_SDK/III.CLEO.h`). CoopIII can resolve it with
  `GetProcAddress` at run time, so there's no link dependency. Without
  III.CLEO nothing gets registered, and nothing else changes. A handler
  receives the `CScript*` and uses CLEO's `Collect`, `Store` and
  `UpdateCompareFlag`.
- **An opcode block.** III.CLEO itself takes 0x05DC-0x0609, 0x0A8C-0x0AFB and
  two single ids (0x0673, 0x0DD5), and its IntOperations plugin takes
  0x0B10-0x0B1D. Retail III's own commands stop at 1154 (0x482), where
  `addresses.h` puts the end of the last range handler. The block from 0x1D00
  up is used by none of them. Pick a block and get it recorded with the Sanny
  Builder library maintainers, which is what CLEO Redux's SDK docs suggest for
  any plugin that gets shared.
- **The Sanny side.** A Sanny Builder 4 mode, meaning a `mode.xml` over the
  stock III data plus a command library with a `CoopIII` extension (class
  `Coop`). That's the whole "SDK for Sanny Builder": autocompletion and
  `Coop.IsHost()` syntax, with no compiler changes.
- **CLEO Redux, for JavaScript.** It supports III 1.0. Installed as an addon
  to III.CLEO, "all custom commands become available to JavaScript runtime"
  (`relation-to-cleo-library.md`). So once their definitions are in its
  `gta3.json`, the same `COOP_*` opcodes can be called from JS or TS with
  `native("COOP_...")`. It's freeware and closed source, so treat it as an
  optional path, not a dependency.

The commands come in two layers:

- **(a) Queries and messages.** These work with every machine running its own
  copy of a script:
  - `COOP_IS_CONNECTED`
  - `COOP_GET_PLAYER_COUNT`
  - `COOP_GET_PLAYER_CHAR`, the local handle of a remote player's ped, or -1
  - `COOP_GET_PLAYER_NAME`
  - `COOP_IS_SESSION_HOST`
  - `COOP_SEND_SCRIPT_EVENT` / `COOP_GET_SCRIPT_EVENT`: a few integers the
    server relays to every copy of the same script

  That's enough for races, tag, deathmatch and co-op score attacks. It doesn't
  depend on §4.2 and can be built first.
- **(b) Session scripts.** A CLEO script marked as a session script runs on
  one machine, and the bridge carries its effects to everybody else, exactly
  as it does for a stock mission. It needs §4.2, and once that exists it costs
  almost nothing.

**Mission overrides are the escape hatch.** If the generic bridge can't make a
good co-op mission out of a stock one, load a replacement into the 32 KB slot
when `0417` launches it. That's one mission compiled with the same SDK, not a
new `main.scm`. It's a lead, not a proof:

- re3's handler for `0417` reads the mission from `data\main.scm` at
  `MultiScriptArray[n]` into `ScriptSpace[SIZE_MAIN_SCRIPT]`;
- III's negative jump targets are relative to that slot;
- that's the same convention III.CLEO gives `.cs` files, which is why it
  re-registers `0002`, `004C`, `004D` and `0050`.

A licensing note: an override written from scratch is ours, but one edited
from the decompilation is still Rockstar's mission.

### 4.4 A scripting language on the server

This is the SA-MP and MTA model: Lua on the server, primitives executed by the
clients. It's good for game modes, since the server is authoritative and it's
testable without the game, which this project values. It's no use for the
stock campaign, which is SCM running inside the engine, and it overlaps layer
(a) above. It's optional, for later.

### 4.5 Every machine plays the mission (rejected)

`campaign.md` §3 rejects running the script on every machine, and the
measurement agrees. Missions branch on physics every frame (`LOCATE_*`, a car
wrecked, a char dead), and N copies of a mission would create N sets of its
peds.

A cleverer variant has the helpers run the mission but take every condition's
result from the owner. That gets the globals and the threads for free. But an
opcode nobody classified then runs twice by default, which is the opposite of
this project's "fail loudly". Noted, not recommended.

---

## 5. The bridge, in more detail

### 5.1 Who runs a mission: the player who started it

- **The owner's machine has the world.** The script's "the player" is the
  protagonist, and the machine of the player standing in the marker has the
  area streamed and the collision loaded. A session host across the river has
  neither, as `roadmap.md` §2.1 and §2.2 say. It's the same reason a
  driverless car's custody goes to its last driver and not to the host
  (`roadmap.md` §5.8.2).
- **It's population.md's rule.** "Whoever's engine made it, owns it." The
  mission's peds and cars are made by the owner's engine, and the owner hosts
  them.
- **Each machine keeps its one mission slot**
  (`bAlreadyRunningAMissionScript`), and only the owner's is used. The session
  has one mission at a time, of whatever kind, and everybody is in it (§9,
  `mission-audit.md` C3).
- **Two terms.** The *session host* keeps owning the clock and the sky
  (`protocol.md` §2.7). The *mission owner* is whoever is running the mission.
  They're often different players.

### 5.2 Starting: arbitrated at `CAN_PLAYER_START_MISSION`, reported at `0417`

Every machine already detects its own player in its own markers. What's
missing is only permission, and that's the pickup shape again: the client
detects, the server arbitrates, the engine starts.

`03EE CAN_PLAYER_START_MISSION` is asked at the story and payphone triggers,
right before the trigger commits to the make-safe, the fade, the title and the
launch. Answering it false costs nothing, because the loop just asks again
250 ms later.

Of its 69 sites in main, 66 lead to a `0417` within forty lines. The other
three are the three safehouse save points (`I_SAVE`, `C_SAVE` and `S_SAVE`),
which ask the same question before opening the save menu. So the claim has to
know which kind of site is asking. The running script's name tells it, and so
does a table of the 66 launch sites built from the script at load. With that
settled:

- the first time it's asked, claim the session's story slot, and keep
  answering false until the grant comes back;
- once granted, answer true;
- while somebody else holds the slot, keep answering false, and have the HUD
  say who is on a mission.

`0417` is where the mission's number becomes known, so the owner reports it
there and the effect stream starts. Odd jobs, RC, the 4x4 runs and Mayhem
never pass through `03EE`. They're session missions too, as decided in
§9. So each one gets the same claim at its own start gate, the last player
check its trigger makes before its `0417` (`mission-audit.md` C1).

Tier 3's "any player can start a mission" therefore needs no trigger
interception at all. It needs one condition answered by the session.

> **2026-09-29.** A contact's or a payphone's start is the host's alone now:
> each machine's trigger is its own save's, which may be behind the host's or
> ahead of it, so a guest's is answered no and claims nothing (§5.8). The odd
> jobs and the RC, 4x4 and Mayhem runs are still anybody's.

> **2026-09-30** (`protocol.md` §1.55). A guest standing in the host's
> contact's marker answers the host's own locate for it, with the host
> within 50 m, so anybody walking in starts the host's trigger (§5.8). And a
> start that is still waiting is held for as long as somebody stands in the
> marker: it used to be let go after ten seconds, or when our last mission's
> end had not reached the server yet, and the trigger then waited for its
> player to walk out and back in.

### 5.3 Mission entities are population entities

- **Extend the host tests.** A `MISSION_CHAR` or `MISSION_VEHICLE` made by
  this machine's own mission script, as opposed to one CoopIII made (the
  `ReplicaScope` already tells them apart at `CWorld::Add`), is announced as
  the owner's, with a mission bit.
- **Hits already find their way.** A player's hit on another machine's ped
  goes to that ped's host (`C_PedDamage`), and a hit on its car goes to the
  car's host (`C_CarHit`). A host's NPC hitting another player goes to that
  player (`C_NpcDamage`).
- **Objects need a small roster of their own**, since missions create them at
  87 sites, often doors and bombs that move.
- **Enemies can be retargeted with existing machinery.** Enemies told to kill
  the player (351 sites) target the owner's ped. `population.md` §6 already
  forwards an NPC's hit on another player's copy, so pointing the objective at
  the nearest participant's ped (on the owner's machine that's a replica) is
  the Tier 3 half, and nothing new is needed for it.

### 5.4 Effects are replayed through the engine's own interpreter

- **On the owner**, a seam at the opcode dispatch records every opcode on the
  replay list, with its inputs as `CollectParameters` evaluated them.
- **On everybody else**, the same opcode runs through the engine's own
  handler, from a small bytecode buffer and a scratch script. This is
  plugin-sdk's `CallCommandById` technique for III: set the script's IP to the
  buffer minus `ScriptSpace`, then call `ProcessOneCommand`. That's roadmap
  §6's "do what the engine does" again: the effect comes from the engine, not
  from a reimplementation of it.
- **Handles are translated.** An entity argument becomes a netId, and the
  netId becomes the local replica. A handle an opcode outputs (a blip, a
  sphere, a cutscene object) goes into a per-mission map from the owner's
  value to the local one. When the owner's output went into a global, the
  local handle goes into the same global, because the main script removes
  contact blips later by reading that global.
- **The list starts small and grows by measured need.** Start with
  `PRINT_NOW`, `PRINT_BIG`, `PRINT_HELP`, `CLEAR_PRINTS`, `ADD_BLIP_FOR_*`,
  `REMOVE_BLIP`, `DRAW_CORONA`, the on-screen timer and counter, and
  `PLAY_MISSION_PASSED_TUNE`. An opcode that isn't on the list isn't replayed,
  so a helper misses something visible. That's a failure you can see and fix,
  not a double effect.
- **Cutscenes are local playback.** Every machine has the same cutscene files,
  so replay `LOAD_CUTSCENE` through `CLEAR_CUTSCENE` with the handle map, and
  freeze the helpers with a replayed `SET_PLAYER_CONTROL`, as `campaign.md`
  §2.3 already wanted.
- **A frenzy inside a mission** (`START_KILL_FRENZY` in DIABLO3 and HOOD1)
  replayed starts every machine's own `CDarkel`, and `rampage.md`'s machinery
  takes over from there unchanged.
- **Nothing off the wire reaches the interpreter unchecked.** The engine's
  handlers trust their operands: an output local is written at whatever index
  it names, a global at whatever offset, a label is read up to its NUL, and a
  weapon, garage, phone, player or particle index goes straight into a table.
  So a participant runs an instruction (and a campaign delta's one, and a
  move) only when it has exactly the shape `replay::Encode` gives it
  (`replay::WellFormed`), its table indexes are inside their tables
  (`replay::OperandsInRange`), and any model it names has model info here.
  `CREATE_OBJECT` of a model that is not loaded asks for it and loads it
  first, as the script would have, and is not made when it still is not in.
- **A cutscene head is made with its animation.** `CREATE_CUTSCENE_HEAD`
  puts the head in the world at once, and only the `SET_HEAD_ANIM` the script
  runs straight after it gives it an animation (every one of the campaign's
  89 is followed by one). Each is a packet of its own and a frame can fall
  between them, so a participant holds the head until the next instruction
  arrives and runs both in the same call; a head with nothing after it is not
  made. The scene's bodies need no such care: `CreateCutsceneObject` keeps
  them out of the world until `START_CUTSCENE`.

### 5.5 Shared progress: a campaign delta

1. **Start from the same state.** The minimum is that everybody starts a New
   Game together, or loads a copy of the same save. Later, the host's save
   goes over the wire and the joiner loads it through the engine's own loader.
   The server refuses a joiner whose campaign hash doesn't match, and says
   why, as it already does for a protocol mismatch.
2. **Stay in step.** When the mission ends, the owner sends the value globals
   that changed and the main-script threads the mission started, by offset.
   Offsets are safe to send because every machine runs the same `main.scm`,
   which gets hashed. Handles don't travel: a handle is a per-process pool
   reference (`protocol.md` §1.5), and the opcode that stored a global says
   whether it holds one. While the mission runs, the owner also sends the
   persistent world opcodes from §3.4 as they happen.
3. **Log it.** The server keeps the deltas as a log. A late joiner loads the
   same start and replays the log.
4. **Check it.** Each machine hashes its campaign globals, and the server
   compares them, in the same shape as `C_DesyncProbe`. A divergence is loud,
   not silent.

**As built** (§15, `protocol.md` §1.30): 2 and 3, and what 1 needs of them.
The values sent are the ones the mission changed, the world opcodes of §3.4
ride the delta as instructions, and every client keeps the log too. Which
part of it a game lacks is worked out from that game's own globals, so a save
from before the session and a save from the middle of it are both brought up
to date, and a load looks again. 1's save over the wire and 4 are not built;
a main.scm that differs is refused per delta, by its hash, not at the join.

The server keeps the log on disk too, in `CoopIII-Progress.dat` next to
`server.exe` (`server/core/progress.h`), with the hidden packages the group
found and the Import/Export and crane lists. It's read when the server starts
and rewritten whole, through a temporary file, within a second of anything in
it changing. The log keeps its name (`S_MissionState::campaignLog`) and its
numbers, so a client that stayed up through a server restart keeps its copy,
and one that joins the next evening asks from 0 and catches up like any late
joiner. Last sitting's slot numbers mean nothing now, so a kept delta has no
owner. The file holds one log, not one per main.scm: the server never sees a
script and can't tell groups apart before their first delta, and each delta
carries its own hash anyway. `keepProgress = false` in `CoopIII-Server.ini`
leaves the file alone; `-forgetprogress`, or Forget progress in the options
window with nobody connected, sets it aside as `.old` and starts over.

### 5.6 "The player" in a condition

> **Corrected 2026-09-24 by [mission-audit.md](mission-audit.md) §1.3.** This
> section used to say a condition could be answered for any participant,
> opcode by opcode. That's unsafe. 42 `if and` blocks test the player twice,
> like "within 20 m of the destination *and* in `$CAR_MEAT1`", and most
> objectives chain across separate blocks. Answered per condition, one helper
> in the mission car and another at the destination would pass a check that
> no single player passed.

The rule chosen instead (`mission-audit.md` C1) is **everybody has to be
there.**

- **At the start**, everybody has to be at the start's area before a mission
  launches. Each launch's start gate is answered false until they are.
  "At" is within 50 m of the area (`MISSION_START_RADIUS_M`, or the
  server's margin when that is wider; 2026-09-30, `protocol.md` §1.55), not
  the checkpoints' 5 m: a friend in his own car across the street or on
  foot round the corner is in the mission, and one a block away is waited
  for. An odd job's is 50 m round its owner.
- **At every checkpoint**, everybody has to be inside the area before the
  mission moves on. A checkpoint is a location check whose area holds one of
  the mission's coordinate blips, the owner satisfying it as written.
- **Inside allows 5 m.** A participant no more than 5 m outside the area
  counts as there, so a friend parked beside the owner does.
- **Not against the clock** (decided 2026-09-25). A checkpoint that waits
  while the mission's timer, or its rivals, keep going makes the mission
  unwinnable: the 4x4 runs, Multistorey Mayhem, a taxi fare, a paramedic's
  drop at the hospital, Turismo. So a checkpoint waits for nobody while a
  countdown is on the owner's screen (`DISPLAY_ONSCREEN_TIMER` up), nor in a
  race (Turismo, Bling-Bling Scramble), an odd job, or an RC, 4x4 or Mayhem
  run: the owner's arrival is the checkpoint, as in single player
  (`CheckpointsWait` in `sdk/include/coopiii/mission.h`). A participant who
  stays more than 150 m from the owner for more than 10 s in one of those is
  brought beside the owner by the late player's move (§11.5), so they can
  go on helping. The story's untimed checkpoints still wait for everybody.
- **For stealth**, anybody can give the game away. That covers
  `HAS_CHAR_SPOTTED_PLAYER`, `IS_PLAYER_SHOOTING_IN_AREA`, and a short table
  of distance checks such as Cutting The Grass's Spookometer
  (`mission-audit.md` R13).
- **Getting into the mission car** is anybody's (2026-09-30,
  `mission-audit.md` R4b): `IS_PLAYER_IN_CAR` is yes for any participant in
  it, at the wheel or riding, except in an `if and` that first asked where the
  owner is. The owner can still do the driving from the passenger seat,
  because the vehicle conditions test "in the car", not "driving"
  (`mission-audit.md` §1.4).
- **Everything else** is the owner's, as protagonist: reaching a character,
  lifting a phone, getting a car of his own (`IS_PLAYER_IN_ANY_CAR`, whose
  answer the script then reads back with `STORE_CAR_PLAYER_IS_IN`).

"Everybody" is safe where "anybody" wasn't: it distributes over an `if and`,
so a compound check still needs one state that satisfies all of it.

Actions are a separate question, and the decisions in §9 answer it: what
the story does to "the player" happens to every participant
(`mission-audit.md` R12). That covers:

- the weapons handed over or taken;
- the wanted levels;
- the control freeze;
- the teleports;
- the pay (§12).

### 5.7 Death, arrest and failure

The owner's death fails the mission, exactly as in single player.
`roadmap.md` §5.4 says any player's death fails it. A mission's top level asks
`0112 HAS_DEATHARREST_BEEN_EXECUTED` after the engine's `DoDeatharrestCheck`
has unwound its gosub stack (70 sites). So a helper's death has to go through
that same unwinding on the owner's script, not just flip a flag. That seam is
a lead (§8).

### 5.8 Whose campaign: the host's (2026-09-29)

In practice one player always hosts, with the save that is up to date, and
the others bring whatever save they have: behind the host's, ahead of it, or
the same. Every machine runs its own save's `main.scm`, so every machine has
its own contacts and its own triggers. The session's campaign is now the
host's save.

**Who the host is.** `Session::HostId` on the server, `S_Welcome` and
`S_WorldState::hostPlayerId` on the wire, `Client::HostPlayerId` on a client:
the first player to connect and, when they leave, the lowest slot still in.
It is the player whose clock the session keeps (`protocol.md` §2.7) and whom
the cheats go to. Not the lobby's host (the launcher's lobby order) and not
the campaign log's: the log has no owner, each delta only says whose mission
it came from, and a kept one from an earlier sitting says nobody. So the host
is the one definition the session already had for "the player running it".

**What a contact is.** Every mission a contact or a payphone gives starts
from two things. The trigger, a main-script thread (Luigi's, Toni's, the El
Burro phone's) that waits on its own save's flags, stands in a locate with
the sphere off (every locate before a `03EE` in main.scm passes 0)
and asks `CAN_PLAYER_START_MISSION`. And the contact: a `BLIP_CONTACT_POINT`
that `ADD_SPRITE_BLIP_FOR_CONTACT_POINT` (`02A7`) made into
`$LUIGI_MISSION_MARKER` and the rest, which the engine draws twice, as the
icon on the radar (`CRadar::DrawBlips`) and as the ring of markers in the
street (`CRadar::Draw3dMarkers`), neither while `$ONMISSION` is 1. The
radar's table is in the save (`CRadar::SaveAllRadarBlips`), so a loaded save
brings its own contacts with it.

**What a guest saw until now**, read off the code and main.scm, not run:

- *The same save.* The same contacts as the host, and either of them could
  start the next mission (§5.2): whoever walked into the marker claimed it,
  and it started once the other was there too. The delta at its end
  brought the other game along (§5.5), the contact it unlocks included
  (12b2eef). This case was fine.
- *A save behind the host's.* The guest's own contacts: the older ones, for
  missions the host passed long ago, and none of the host's newer ones except
  what the campaign log had brought (only missions played on this server,
  `CoopIII-Progress.dat`). Walking into an old marker claimed the session's
  mission from the guest's own trigger; with the host standing there too it
  started, on the guest's machine, and its delta went into the log and into
  the host's game. A delta holds every global the mission wrote, its own
  scratch ones included, so the host's game, which has long passed it, would
  in general not already hold all of it and applied it: an old contact made
  again into its global, old values over the host's, its threads started.
- *A save ahead of the host's.* The guest's own contacts for missions the
  host has not reached, and the same claim: starting one ran it on the
  guest's machine and its delta moved the host's campaign ahead, a piece of
  it out of order. The other way the log's deltas of the host's missions,
  which the guest had passed, were applied to the guest's game for the same
  reason, contacts the guest's later missions had taken away made again.

**Now** (`protocol.md` §1.49): on a machine that is not the host, while the
server shares missions and its own `missions = on`:

- its own contacts are not drawn, icon or markers, and its own story and
  payphone triggers are answered no at `CAN_PLAYER_START_MISSION` and claim
  nothing: the trigger waits for its player to walk out, as it does for any
  no. The odd jobs, the RC, 4x4 and Mayhem runs, whose start is the van or
  the car and not anybody's progress, are anybody's as before.
- the host's contacts are drawn instead, as the host's radar has them: the
  host reads its table twice a second and sends it when it changes. They are
  drawn the engine's way, `DrawRadarSprite` and `C3dMarkers::PlaceMarkerSet`
  with the arguments `Draw3dMarkers` uses, and not while the guest's own
  `$ONMISSION` is 1, which the session's mission holds it at.
- behind, ahead or the same makes no difference: nothing of the guest's own
  progress is looked at.
- the host's places are drawn beside the guest's own (2026-09-30,
  `protocol.md` §1.57): the Ammu-Nations, bomb shops, Pay'n'Sprays and
  safehouses its campaign has put on its radar, each one the guest's radar
  has no icon of nearby, on the radar only. A guest whose save has not
  unlocked Staunton's Ammu-Nation sees it once the host has.

The story is started by the host, and everybody else walks into the host's
marker to be at the start, as before.

**Anybody in the host's marker starts it** (2026-09-30, `protocol.md`
§1.55). The host's trigger asks whether the host's own player is in the
marker, so a guest standing in it used to count for nothing until the host
walked in too (Farewell 'Chunky' Lee Chong, in a playtest). Now the host's
machine answers that locate yes for a guest: when the locate is one of the
host's contacts' (its box, 3 m round, holds a contact the host sent), the
host's player is within 50 m of it, and a guest in the session is inside
it. The rest is the host's own start, unchanged: its 03EE, its claim, which
waits for everybody within 50 m, its fade and title, and the mission on the
host's machine. The guests' own triggers still start nothing. Marty's
payphone and Give Me Liberty's starts have no contact on the radar, and
still need the host there. The host a block away starts nothing: the
opening would run with its player wherever he is.

**The guest's save.** Nothing of this goes into it. Its own contacts stay in
its radar's table and are only left out of the two draws: for the length of
each call their display reads `NEITHER`, and it is put back as the call
returns. The host's are never in its table. A save the guest makes in a
session is its own game's, contacts and all. What does go into a guest's game
is what always did, by design: the campaign log's deltas (§5.5), which are
the session's missions' globals, contacts and threads. Since only the host
starts a story mission now, those are the host's missions. A guest's save
from a session keeps them.

**A save ahead never goes back** (2026-09-29, `protocol.md` §1.30). The ahead
case's hazard above, a delta of a mission the guest had passed applied over
its later state, is closed by what main.scm itself says a pass is. Every
passable story mission sets a flag of its own to 1 ($LUIGIS_GIRLS_COMPLETED
and the rest) that nothing ever puts back: the new game's main script sets it
to 0 once, the contact's trigger compares it and ends itself on it, the
mission sets it to 1 once. The owner tells these latches from the code it
has, main.scm's and its mission's (`game/mission.h`, `IsLatch`), and marks
each one it took from 0 to 1 in the delta (`CAMPAIGN_VALUE_LATCH`). A machine
whose own game already holds every latch a mission's delta marks has passed
that mission in its own save, and applies none of its parts: not its
globals, not its world instructions (a contact made again), not its threads.
Its later state stays as it is, in the session and in a save it makes there.
A mission it has not passed, or has passed only part of the way, is applied
as before, so a save behind the host's and the same save are unchanged. The
parts of one mission are judged together, before any of them is written, and
a mission's first part waits for its last.

`clienttest` holds the rule to the retail main.scm: every mission whose owner
would mark a latch marks one that no other mission writes, so holding every
marked latch means holding that mission's own; and every mission that
registers a pass marks one, except the four RC runs, whose flag only their own
mission reads (as does the odd jobs' siren help, which two missions set, and
from one mission's code the two can't be told apart). What is not covered,
and applied as before: the RC runs' first pass, a repeat of an odd job, RC,
4x4 or Mayhem run (a record the guest beat is overwritten by the host's), a
failed mission's delta, and deltas kept from a build before this one
(`roadmap.md`, known issues).

**Leaving.** Out of the session there is no host: the guest's own contacts
are drawn again at once, and its triggers start its own missions, as before
it came. Whatever the log applied while it was in stays. When the host
leaves and the rest stay, the next host's save is the session's campaign:
their contacts are drawn once their machine has sent them, within about a
second, and their story starts are theirs.

**Limits.** A host whose own `missions = off` sends no contacts and claims
nothing, so the guests have no contacts and no story start. A server that
does not share missions leaves every machine its own, as before.

---

## 6. Rampages and the rest of the side content

| Content | Where it lives | Today | With the bridge |
|---|---|---|---|
| 20 rampages | `rampage.sc`, a main thread | shared, scaled or off; built and never run in a game | unchanged |
| a frenzy inside a mission | DIABLO3, HOOD1 | the owner's alone | replay `START_KILL_FRENZY` (§5.4) |
| 100 hidden packages | `packages.sc` | shared or per player, built | unchanged |
| unique jumps, insane stunts | `usj.sc`, `hj.sc` | per player; works, and the slow motion is the jumper's own `SET_TIME_SCALE` | optionally shared, like packages |
| the four odd jobs | missions 11-14 | private | a session mission like any other, with everybody in it (§9); others see the fares and the criminals as mission entities |
| RC Toyz and Toyminator | 3-6, 75 | private | the owner's RC car as an ordinary replica, never `STATUS_PLAYER_REMOTE` on anyone else (`protocol.md` §1.4) |
| 4x4 runs, Mayhem | 7-10 | private | owner-run checkpoint races, with everybody at every checkpoint (`mission-audit.md` C1); a real co-op race is an SDK script |
| Import/Export, the emergency crane | `import.sc` | the garage doors are shared; the lists were per machine | the session's campaign (§6.1) |
| story and payphones | 15-79 | private | the bridge |
| intro, info scenes | 0-2 | local, with one catch (below) | stay local |

**The catch with the info scenes.** Missions 1 and 2 aren't started by a
marker. They start when the script's `0214 HAS_PICKUP_BEEN_COLLECTED` sees
the info pickup outside the hospital or the police station collected, with
the player in Portland View and not on a mission. The pickup work treats every
pickup that isn't a mine as exclusive (`IsLive` in `pickup.cpp`), and it
pushes somebody else's collection into this machine's `aPickUpsCollected`.
So one player walking into the hospital's info icon should start the hospital
scene on every machine whose player is in Portland View at that moment. That's
the free mechanism that makes rampages work, pointed somewhere nobody meant it
to go. It follows from the code and hasn't been seen in a game. If it shows
up, the fix is to leave those two pickups local.

### 6.1 The Import/Export lists and the crane, as the session's

> **Built 2026-09-25.**

The two Import/Export boards (Portland's harbour and Shoreside Vale) and the
emergency crane's list are not script globals. They are the engine's own
bits: `CGarages::CarTypesCollected`, a word of sixteen for each collecting
garage, and `CCranes::CarsCollectedMilitaryCrane`, seven (`addresses.h` has
the witnesses). The garage takes only a car its bit says is still wanted,
pays its $1,000 there and then, and a save keeps the bits. The script
threads `IMPORT1`, `IMPORT2` and `M_CRANE` only ask the engine (`03D4`,
`03EC`), and tick the board, add the progress and open the crane with its
$200,000 when their own player comes by.

So the campaign delta was not the tool: it carries the globals and world
instructions a *mission* leaves, and these change outside any mission, in
engine memory. What was built is smaller:

- each machine reads its bits twice a second and, when it has one the
  session lacks, sends them all (`C_CarLists`); it asks once on every
  connection too;
- the server keeps every machine's put together (nothing is ever taken off
  a list) and sends that back, and to everybody when it grew (`S_CarLists`);
- each machine ORs the session's bits into its engine's words. It never
  calls the engine's marking function, so nobody is paid for a car somebody
  else brought. Its own `import.sc` then does what it does for a car of its
  own, the next time its player is at the garage or the crane.

The money goes by the money rules: the car's $1,000 goes to whoever brought
it (into the one wallet under `money = shared`), and the crane's $200,000 is
every machine's own script paying its own player, as a shared rampage's
reward is (`moneysync.h`). A load or a new game that lacks the session's
bits gets them back, as the campaign log does. Deleting what the garage and
the crane take is a separate matter, done on one machine (the car removal
work in `vehicle.cpp` and the garage code); this is only the lists.

Checked again 2026-09-29 (`pickups.md` §13.3): both words are what
`CGarages::Save` and `CCranes::Save` write, so every save made on any machine
after a delivery has the whole session's lists, and a joiner with an older
or a newer save ends up with the union of his and the session's. The hidden
packages now do the same (`pickups.md` §13.2).

For rampages specifically:

- **Run them.** They're built and have never been run in a game.
- **Close `rampage.md` §7 and §9.5.** Those are the stats screen, the first
  round trip, the police helicopter, and a machine whose own frenzy already
  ended.
- **Add a per-player tally.** The server already sees who reported each kill,
  so the F9 list or the chat can show it at the end.
- **Versus rampages belong to the SDK, not to `CDarkel`.** `CDarkel` is a
  singleton and `roadmap.md` §5.10 already settled that per-player frenzies
  aren't a `CDarkel` feature. A versus mode is a CLEO script on every machine,
  counting with `COOP_SEND_SCRIPT_EVENT`.

---

## 7. The plan

**Phase 0: prove the seams.** This is small and ships no feature:

- run the rampages in a game;
- replay one opcode: a `PRINT_NOW` from one machine, drawn on another through
  the engine's own interpreter, with III.CLEO installed;
- register one `COOP_*` opcode through `CLEO_RegisterOpcode` and call it from
  a `.cs` compiled in Sanny Builder;
- prove the §8 list.

**Phase 1: help a friend.** Everybody keeps their own progress:

- mission entities join the population roster (§5.3);
- the first replay list (§5.4);
- "X started Y" and "X passed Y" on the HUD.

Done when two players play Luigi's "Don't Spank Ma Bitch Up" together, and the
helper sees the target, his blip and the objective text, and can kill him.

**Phase 2: a shared campaign.** This adds:

- the same start;
- the story slot at `03EE` (§5.2);
- the campaign delta, its log and its hash (§5.5);
- cutscenes for everybody;
- the death rule (§5.7).

Done when two players pass Luigi's first three missions and both machines'
main scripts then offer the same next missions, with equal campaign hashes.

**Phase 3: everybody in the fight.** Detection answered for every participant
(§5.6) and enemies that target the nearest participant (§5.3). The
objectives stay the owner's.

**Phase 4: the SDK.** Layer (a) can start any time after Phase 0, since it
doesn't depend on Phases 1 to 3. Layer (b) waits for Phase 2 (§4.3).

**Phase 5: overrides**, for the few missions that need them (§4.3).

---

## 8. Leads to prove before building

These all come from III.CLEO's `source/III.VC.CLEO/Game.cpp`, in its
`GAME_V1_0` branch. They're CLEO's reading of the retail image, and on the
target install they're live patches:

- **`RedirectJump(0x439500, ...)`.** `addresses.h` records `0x00439500` as the
  opcode dispatcher, and `campaign.md` §4 and §6 name it as Tier 3's
  intercept point. On the target install its first bytes are CLEO's jump.
  MinHook can chain over a jump, but this is the hottest function in the
  script engine and CLEO owns it. Hook the range handlers or the individual
  handlers instead.
- **`RedirectJump(0x4382E0, ...)` and `RedirectJump(0x438460, ...)`.**
  `CollectParameters` (`CTheScripts__CollectParameters` in `addresses.h`) and
  its no-advance twin are CLEO's too.
- **`RedirectJump(0x4386C0, ...)`, `SetPointer(0x438809, ...)` and
  `SetInt(0x43882A, sizeof(CScript))`.** CLEO swaps in its own script array,
  of 0xB0-byte scripts instead of the engine's size. Never index the engine's
  script array. Walk the active list at `0x8E2BF4` instead.
- **Script lifecycle hooks.** There are `RedirectCall`s at `0x453B43`,
  `0x48C26B` and `0x48C575` (script init on load, new game and reinit),
  `0x48C4A2` (shutdown) and `0x58FBD9` (saving the scripts). The campaign
  delta needs the same moments.
- **Range handlers**, by CLEO: `0x439650`, `0x43AEA0`, `0x43D530`,
  `0x43ED30`, `0x440CB0`, `0x4429C0`, `0x444B20`, `0x4458A0`, `0x448240`,
  `0x44CB80`, `0x588490` and `0x589D00`. `addresses.h` agrees on 200, 500,
  600, 800 and 1100. The dispatcher's call puts 100-199 at `0x43AEA0`; the
  ped lifecycle note's `0x0043AEA4` was four bytes into it, and is fixed.
- **Script data.** `ScriptParams` at `0x6ED460` matches `addresses.h`.
  `ScriptSpace` at `0x74B248` and the active list at `0x8E2BF4` are CLEO's.

Not yet located:

- the handlers for `03EE` and `0417`, both in the 1000-1099 range;
- the handler for `0112`, in the 200-299 range;
- `CRunningScript::DoDeatharrestCheck`, which the per-script `Process` calls
  and which no range table reaches.

The three handlers come out of the range tables above.

re3's account of `0417` (reading the mission from `data\main.scm` into the
slot) needs checking against retail before any override depends on it.

---

## 9. What was decided, and what's still open

Decided on 2026-09-24, after reading the sections above:

1. **The target is a shared campaign.** The whole game, played together from
   start to end, with one progress for everybody. Phase 1 stays in the plan
   as the step on the way, not the goal.
2. **Everybody gets the reward.** A mission's `ADD_SCORE` pays every
   participant (§12).
3. **A death fails the mission, behind a server switch.** The switch already
   exists: `missionFailOnDeath` in `server/core/config.h`, default `true`,
   which is `roadmap.md` §5.4. Nothing reads it yet (§12).
4. **More players should mean a harder mission,** without touching
   `main.scm` (§10).
5. **Everybody takes part in the mission, not only the one who started it.**
   Everybody sees the cutscenes (§11).

6. **The rule for everything else: if vanilla doesn't allow it, neither do
   we.** That's `roadmap.md` §5.5, made explicit for missions. Applied to
   what was open:
   - **Odd jobs during a story mission: refused.** Single player can't start
     one while `$ONMISSION` is set, and every participant's `$ONMISSION`
     mirrors the session's mission (§11.4).
   - **Saving during a mission: refused**, by the same mirror. The three save
     points check `$ONMISSION` themselves.
   - **Rampage pickups during a mission: refused**, the same way.
     `CanBePickedUp` refuses a `KILLFRENZY` while `IsPlayerOnAMission`.
   - **Cutscene skipping: the mission owner skips, and everybody follows.**
     Single player lets the player skip, and the owner is the mission's
     player. A vote isn't vanilla. The participants' own skip does nothing.
   - **Difficulty scaling: off by default.** Vanilla has one difficulty, so
     `missionEnemies` defaults to `original` and scaling is a switch a server
     turns on (§10.4). That's §5.5 again: server options may relax fidelity,
     the defaults don't.
7. **Everybody at the start and at every checkpoint** (§5.6,
   `mission-audit.md` C1). That settles the sub-missions too. A mission only
   starts with everybody there, so there's one at a time and everybody is in
   it, odd jobs included (`mission-audit.md` C3).
8. **The stash is for everybody.** Every weapon, armour, health and cash pickup
   a mission lays out is per player. Nobody's copy disappears when somebody
   else takes theirs (`mission-audit.md` R2).
9. **Everybody has to be quiet.** The stealth checks are answered for the
   participant nearest the danger (`mission-audit.md` R13).
10. **The mission's HUD widgets are on everybody's screen**, with their values:
    the timers, the counters and the bars, the Spookometer among them
    (`mission-audit.md` R16).
11. **There means inside the area, or no more than 5 m outside it**, at the
    start and at every checkpoint (`mission-audit.md` C1).
12. **A player who never comes is for the host to kick**, from the player
    list in the game (`mission-audit.md` §6). Built: `/kick` and the list's
    number, in the chat (`protocol.md` §1.28).

Still open:

1. **CLEO as the SDK's dependency.** III.CLEO would be required for `.cs`
   scripts, and CLEO Redux optional for JS.
2. **The opcode block** for the `COOP_*` commands.

---

## 10. Difficulty for more than one player

The owner's worry is that `main.scm` fixes how many enemies a mission has, so
two players would breeze through it. That's true of the script and not of the
engine. Every enemy a mission makes goes through CoopIII-visible seams on the
owner's machine, and CoopIII can make it tougher, or give it company, without
the script ever knowing.

### 10.1 Which missions it's about

32 of the 80 missions give somebody a `KILL_PLAYER` objective. Those are the
fights, and they're where difficulty scaling means something. The biggest:

| Mission | KILL_PLAYER sites | Peds created |
|---|---|---|
| ASUKA1 Sayonara Salvatore | 112 | 23 |
| LOVE4 Grand Theft Aero | 27 | 20 |
| LOVE5 Escort Service | 24 | 28 |
| ASUKA2 Under Surveillance | 18 | 10 |
| RAY6 Marked Man | 17 | 18 |
| DIABLO2 I Scream, You Scream | 16 | 5 |
| TONI3 Salvatore's Called A Meeting | 12 | 13 |
| CAT1 The Exchange | 12 | 29 |
| ASUSB1 Bait | 12 | 18 |
| TONI4 Triads And Tribulations | 9 | 11 |

The other 48 are driving, delivering, chasing and escorting. Co-op makes those
easier by nature (one drives, one shoots), and nothing below changes them.
28 missions run an on-screen timer, which is the one other lever that exists,
and §10.3 leaves it alone on purpose.

### 10.2 The levers, in the order to build them

1. **Tougher enemies.** When the owner's mission script gives a char a
   `KILL_PLAYER` objective (`01CA`/`01CC`, 351 sites), CoopIII raises that
   ped's health and armour by a factor that grows with the number of
   participants, once, through the engine's own `SET_CHAR_HEALTH` and
   `ADD_ARMOUR_TO_CHAR` handlers run through the replay interpreter (§5.4).
   Nothing about the script changes: an enemy the script is waiting for dies
   later, and the script's `IS_CHAR_DEAD` still finds out the ordinary way.
   It's the cheapest lever and it covers every fight.
2. **More enemies, that count like the originals.** At the same seam, the
   owner's machine creates extra peds next to the one the script made: same
   model, same weapon, same objective, through the engine's own `CREATE_CHAR`
   path, and each one added to the mission's cleanup list. They're not extra
   decoration. They're the same enemy, several times over: if the mission
   waits for the original to die, it waits for every copy; if the original
   gets a blip, every copy does. §10.5 is how, without the script knowing.
   Capped, because the ped pool is shared with the city and ASUKA1 alone hands
   out 112 objectives.
3. **Harder police.** Missions that raise the wanted level
   (`ALTER_WANTED_LEVEL` 17 sites, `ALTER_WANTED_LEVEL_NO_DROP` 31) do it to
   the owner. Replaying them to every participant makes the heat shared, which
   is also what `roadmap.md` §5.1's vehicle rule would do to anybody riding
   with the owner anyway.

### 10.3 What not to scale

- **Timers.** A timer is a global the script counts down itself. Shortening
  it means writing into the script's variables, and a mission that expects a
  time to be reachable alone stays reachable, and a race stays a race.
- **Named characters.** A special character (anybody the mission loads with
  `LOAD_SPECIAL_CHARACTER`, a `SPECIALnn` model: Salvatore, Lips, Kenji...) is
  never copied. There's one of each in the story. Only the generic ones, gang
  members, guards and hitmen, get copies.
- **Rampages.** They already have `rampages = scaled` (`rampage.md` §4).

### 10.4 Server settings

Two, in the shape of the existing ones:

| Setting | Values | Default |
|---|---|---|
| `missionEnemies` | `original`, `tougher`, `more` (tougher and extra peds) | `original` (§9, the vanilla rule) |
| `missionScale` | the percentage each extra participant adds | 50 |

`original` is single player, exactly. With `tougher` and the default scale,
two players face enemies with 1.5× the health, three with 2×.

**Built** (2026-09-24): lever 1, and lever 3 as the replayed wanted levels
(R12). `missionEnemies` and `missionScale` are in CoopIII-Server.ini and reach
every client in `S_MissionState`; the owner's machine raises each enemy's
health and armour once, as its mission gives it a `KILL_PLAYER` objective.
Lever 2 too, as §10.5 says: `more` gives each generic enemy its copies.

### 10.5 A copy counts like the original

The script only has the original's handle. It never learns the copies exist,
and it doesn't need to. The original's handle becomes a *group*: the original
plus its copies. Every opcode the owner's mission script runs on that handle
is answered for the group:

| Kind of opcode | Examples | Answered as |
|---|---|---|
| Is it dead | `IS_CHAR_DEAD` (1,758 sites in the missions) | dead only when every member is |
| Where is it, what is it doing | `GET_CHAR_COORDINATES`, `LOCATE_CHAR_*`, `IS_CHAR_IN_AREA_*` | the group's *representative* |
| Orders and state | `SET_CHAR_OBJ_*`, `GIVE_WEAPON_TO_CHAR`, `SET_CHAR_HEALTH`, `SET_CHAR_PROOFS` | run on every live member |
| Blips | `ADD_BLIP_FOR_CHAR`, `CHANGE_BLIP_*`, `REMOVE_BLIP` | one blip per live member, removed together |
| Letting go | `MARK_CHAR_AS_NO_LONGER_NEEDED`, `DELETE_CHAR` | every member |

The representative is the original while it lives, and after that any live
copy. That matters. Missions guard their orders with `if not IS_CHAR_DEAD`,
so once the group reports "alive" while the original is a corpse, the next
order has to land on somebody standing up.

**The seam is one function.** Every char opcode handler turns the script's
handle into a ped through `CPools::GetPedPool()->GetAt` (`0x0043EB30`,
already in `addresses.h`). While the owner's mission script is the one
running, a detour there can hand back the representative for a group handle.
That covers the "where is it" row for every opcode at once, with no list to
keep. The other rows need a short list of opcodes each: `IS_CHAR_DEAD` for
"dead", about twenty for orders and blips. Those are run once per live member
through the replay interpreter (§5.4). Outside the mission script the detour
passes straight through, so nothing else in the engine sees a group.

Blips on the other machines come free. They're replayed opcodes like any
others (§5.4), and each one names a member that's a replicated mission
entity (§5.3).

Two limits keep it honest:

- **Enemies in a car.** An enemy created inside a car
  (`CREATE_CHAR_INSIDE_CAR`, 73 sites) gets copies only in the car's free
  seats, and none when there aren't any.
- **The cap.** When the pool is too full to take a copy, the group is just
  the original and the mission plays as vanilla.

**Built** (2026-09-24), with these limits. The copies are made where the
original is told to kill the player, one for each player past the first and
three at most, beside it, clear of the buildings, with its model, ped type,
weapon and toughness, each going for whichever player is nearest it. Never
for a player, a cop, a medic, a fireman or a `PEDTYPE_SPECIAL` (every
`SPECIALnn` the missions make is one), never for one in a car, and never when
fewer than 24 of the ped pool's slots are free. The seam is
`CPool<CPed>::GetAt` itself (`0x0043EB30`), found again in
`ADD_BLIP_FOR_CHAR`'s handler before it is hooked, the first time a mission
gets copies, and it answers a group's handle only while the owner's mission
runs an instruction. So `IS_CHAR_DEAD` and every "where is it" instruction ask
the group, and an order lands on the representative alone: the copies keep
going for the players. A blip stays on the original. `DELETE_CHAR` and
`MARK_CHAR_AS_NO_LONGER_NEEDED` on the original let go of its copies too, and
the mission's end lets go of all of them after the engine's own cleanup has
let go of the originals.

---

## 11. Everybody in the mission

### 11.1 What a mission's opening actually does

JOEY1, Mike Lips' Last Lunch, is typical of the 66 missions that open with a
cutscene. In order, on the machine running it:

1. `LOAD_SPECIAL_CHARACTER` for Joey and Misty, `LOAD_OBJECT` for the heads
   and the door, `LOAD_ALL_MODELS_NOW`, then a loop on `HAS_MODEL_LOADED` and
   `HAS_SPECIAL_CHARACTER_LOADED`.
2. `LOAD_CUTSCENE 'J1_LFL'` and `SET_CUTSCENE_OFFSET`.
3. `CREATE_CAR` for the car in the scene, `CREATE_CUTSCENE_OBJECT` and
   `SET_CUTSCENE_ANIM` for the player, Joey and Misty, `CREATE_CUTSCENE_HEAD`
   for the faces.
4. `SET_VISIBILITY_OF_CLOSEST_OBJECT_OF_TYPE` to hide the real garage doors.
5. `CLEAR_AREA`, then `SET_PLAYER_COORDINATES` and `SET_PLAYER_HEADING` to put
   the player next to the scene.
6. A fade in and `START_CUTSCENE`, then a loop on `GET_CUTSCENE_TIME` that
   prints each subtitle at its time.

The player in a cutscene is a cutscene object with the `PLAYER` animation,
not the player's ped. So a helper who plays the same cutscene sees Claude in
it, which is what everybody should see.

### 11.2 Gathering

> **Changed 2026-09-24.** Everybody now has to be at the start before a
> mission launches (§5.6), so nobody needs bringing to it any more. Moving everybody along with the mission's own teleports stays.

Every participant is already beside the owner when the mission launches
(§5.6's start gate). After that, each `SET_PLAYER_COORDINATES` the mission
runs on the owner (82 sites in 49 missions) moves the participants the same
way: to spots around the owner's new position, controls frozen, a fade over
the move. That's what puts everybody where the play starts after a cutscene,
and at the next scene after the next one.

A participant in a car is moved car and all only when the owner was in a car
as well and the participant's machine is the one simulating it; a move of the
owner on foot leaves a car where it is, with its driver in it when the spot is
within 60 m and out on foot beside the owner when it is further
(`protocol.md`, "Moving everybody with the mission"). Give Me Liberty puts the
owner in the safehouse room behind the door, and a car on the ring round that
spot is in the walls.

It also solves a problem nobody would see until it bit. A cutscene plays at a
place, and a machine whose player is across town doesn't have that place
streamed or its collision loaded (`roadmap.md` §2.1, §2.2). The start gate and
the checkpoints mean every machine is already where the cutscene is, and the
moves keep it so after the mission's own teleports.

Who is a participant: every player in the session, but for a game in a
mission of its own, such as a new game's intro, until it is out (§11.6). The
start gate makes sure they're all there.

### 11.3 Cutscenes for everybody

Every machine has the same cutscene files, so a cutscene is local playback.
The steps in §11.1 are replayed to every participant through §5.4, with the
handles mapped. Two details keep it from falling apart:

- **Wait for everybody to load.** The owner's script sits in a loop on
  `HAS_MODEL_LOADED` and `HAS_SPECIAL_CHARACTER_LOADED` before it starts. The
  owner's machine keeps the script in that wait until every participant says
  its models are in, with a timeout of 5 s, so nobody's cutscene starts a
  second behind. That's `rampage.md`'s `ReadStatus` hold again: keep the
  script in its own wait loop until the session agrees. **Built:** the
  owner's `LOAD_ALL_MODELS_NOW` carries a number, a participant's replay of
  it loads everything before anything after it runs, and the participant
  says so (`protocol.md` §1.29). The condition is held, run again next frame,
  rather than answered, since the loop asks under a NOT and an OR.
- **Subtitles go by cutscene time, not arrival time.** The owner prints each
  line when its own `GET_CUTSCENE_TIME` passes a mark. With the wait above,
  every machine's scene starts within a network trip of the owner's, and so
  does every line, which is close enough that nothing more is done for them.

A player with the pause menu open when the session's mission starts for him,
or when one of its cutscenes starts, has it shut, the way the menu's own
Resume shuts it (2026-09-30, `protocol.md` §1.55): the menu does not stop
the world in a session, and he used to watch the menu while the scene ran
behind it. The safehouse's save menu is left alone.

While the cutscene runs, other players' peds are hidden on every machine, or
they'd stand in the middle of the scene. A skip is everybody's
(`protocol.md` §1.34): with more than one player in the scene, the skip input
is a vote, and once 75% of them, rounded up, have pressed it the scene is
skipped on every machine at once. The owner's skip is what moves the mission
on, and its `CLEAR_CUTSCENE` still reaches everybody and ends their replay. A
participant alone in the count does nothing with the skip input, as before
(§9); the buttons the engine skips on are only made to look held when the
client could not take the engine's skip call. The same count covers a scene
each game plays for its own script, a new game's intro above all: nobody
leaves it before the others unless they all agree.

### 11.4 During the mission

- **Everybody is on the mission.** Every participant's `$ONMISSION` mirrors
  the session's mission, so nobody's own markers, rampage pickups or save
  points open while it runs. That's exactly what single player does to one
  player, done to everybody (§9).
- **The mission's blips and text reach everybody** (§5.4).
- **The objectives are the owner's; everybody else fights, drives and
  protects** (§5.6).
- **Enemies fight everybody** (§5.3).
- **Seats.** 15 missions have a ped get into the player's car as a passenger
  (`SET_CHAR_OBJ_ENTER_CAR_AS_PASSENGER`, 43 sites): Misty, Luigi's girls,
  the paramedic's patients, the taxi's fares. A participant riding in the
  owner's car can take the seat the mission needs, and the ped then stands on
  the pavement forever. The bridge has to keep those seats free, or move a
  participant out of one when a mission ped heads for it. **Built**
  (mission-audit.md R4), taking out only as many as the ped needs.
- **The car the mission puts its player in.** When the mission warps the
  owner into a car or a boat, the participants on foot get its free
  passenger seats, nearest first, and the rest follow in their own. On the
  water the owner's checkpoints don't wait for anybody without a boat, since
  there is no reaching them (`protocol.md` §1.40).

### 11.5 Somebody who comes in late

A player who joins while the mission runs, or comes back from a dropped
connection, is in it (§15), and has missed everything the mission put up
before they came. So the owner's machine keeps what of it still stands, in
the order it went out, and hands it to them alone:

- the blips, with the last colour, scale, dimming and display each was given;
- the pickups nobody has taken, and the whole stash, which stays everybody's
  own until the mission ends (R2);
- the fires;
- the objects, where they are now and turned the way they are now, with the
  last collision, dynamics and flashing each was given, and the blips on them;
- the HUD's timer and counter, whose values follow at once.

Whatever the mission took away is not kept, and a change made again replaces
the one before, so what is handed over is the mission's picture now and not
its history. A blip on a pedestrian or car the session no longer names is left
out. Whatever of it reached the newcomer anyway, sent to everybody while they
came in, their machine skips.

Not handed over: what the story did to the player (the weapons, the wanted
level), a cutscene, a fade or a camera in progress, which the next one sets
anyway, and what the world keeps, which the campaign delta brings at the end
(§5.5).

**They are brought to the owner** (built 2026-09-25). Whoever comes in late
is somewhere else: where a joiner's save put them, where the connection
dropped, at the hospital after a death the server's `missionFailOnDeath = off`
let through, maybe on another island. Every checkpoint would wait for them, and
the islands are shut for most of the story, so they may have no road there. So
their own machine moves them beside the owner, three seconds after they came
in, once they are alive and out of any cutscene: out of any car, on foot, in a
place of their own round the owner, the other island's collision and the scene
loaded first. That is the rampage vote's move (`game/rampagevote.h`), which
already does all of it. Somebody within 60 m of the owner is left to walk, and
a mission that started with everybody at its start moves nobody. Leaving them
out of the count until they arrive was the other way; it was not taken, for
the islands. A fire the fire brigade put out on the owner's machine is lit again
for the newcomer, since the mission itself never asks whether it is out.

**Never onto another player's car** (2026-09-28, the Fuzz Ball). The same move
brings along a participant who fell behind in a mission whose checkpoints don't
wait (§5.6). In The Fuzz Ball the owner drives a taxi round the girls; a
participant driving his own car far off was taken out of it and put down 3 m
from where the taxi had been a snapshot earlier, and stood on its roof. Now:
the move waits while the owner's car goes faster than a walk (the Fuzz Ball
stops at every girl), and wherever it puts somebody down is never a place a
car another player sits in covers, nor the road it covers in the next two
seconds, on a car-sized ring (`rampagevote.h`, `MoveKeepOut`). The owner's
`SET_PLAYER_COORDINATES` made in a car gives a participant on foot the car ring
too, and never the owner's spot (`mission.h`, `MoveRadii`,
`MayStandOnOwnersSpot`).

**Whose car the girls get into** (changed 2026-09-30). Every player drives
his own car and picks up girls on his own; the count and the timer are the
owner's HUD, on everybody's screen as before. The owner's script asks, for
each girl, whether "the player" is near her in a car (00FD), stopped (029F),
in a car (00E0), which car (00DA) and then makes him her leader (01DF). When
the owner's answer to 00FD is no and a participant sits in a car inside the
same 8 x 8 x 2 box, the rest of that girl's pick-up is answered for him:
his car stopped, his car's handle (the owner's copy of it), and his copy made
her leader with `SET_CHAR_AS_LEADER` in place of the owner. The engine's own
`UpdateFromLeader` then puts her in his car on the owner's machine, which is
where she rides on every screen, and `IS_CHAR_IN_PLAYERS_GROUP` (0320) reads
the participant leading her as the player. At the station her own branch
clears the leader, gets her out of his car, walks her in and counts her. Her
marker comes off through the script's own `REMOVE_BLIP`, so a girl one player
has picked up is off everybody's radar, and a girl already following somebody
is nobody else's to pick up. Who is answered for is forgotten at the next
`wait` and at the next girl's first question, so nothing else about "the
player" is ever answered for anybody but the owner (`game/fuzzball.h`).
A participant far from the owner is not brought to him in this mission
(`PlayersSplitUp`): each is somewhere else on purpose.

Not proven in a game: that the owner's copy of a participant's car reads
"stopped in the cube" at the station (01A8), which is what gets her out.

### 11.6 A game of its own

> **Built 2026-09-24.**

Not every mission script a machine runs is the session's. A new game opens
with the intro, mission 0, and a first death or arrest with the hospital's or
the police's info scene, 1 and 2, each on the machine it happens to. None of
them is anybody else's. While one runs, that machine's player is in a
cutscene, and its `$ONMISSION` belongs to its own mission.

main.scm makes a new game's player on Give Me Liberty's marker by the bridge,
and the intro keeps them on it or beside it, unseen, for most of its length.
So until this, the first player through the intro could start Give Me Liberty
with everybody else counted as there, in the middle of their own intro.

So a machine whose game runs a mission script that is not the session's says
so (`protocol.md` §1.29, `C_MissionBusy`), and until it says it is out:

- **It is nowhere.** A start waits for it, as for anybody away from the
  marker, and the log names it among the missing. The intro ends with its
  player on the marker, so the first mission waits at the bridge until
  everybody's intro is over, and starts with everybody in it. The
  launcher's lobby starts everybody's new game at once (`protocol.md`
  §1.31), so nobody waits there long.
- **It is out of the running mission.** It runs nothing of it, its death
  fails nothing, and its `$ONMISSION` is left to its own mission. Once that
  is over it comes into the session's mission as a late joiner does (§11.5)
  and is handed what it has up.
- **The owner's game going into one ends the session's mission,** as the
  owner leaving does (§12.3): a new game or a load in the middle of the
  mission takes its script with it.

A game that starts over, from a load or a new game, is told by the engine's
frame count going back (`CTimer::m_FrameCounter`). The game clock would be
the wrong witness, since a hitch can look like a restart. A participant's game
that starts over forgets whatever the mission put into it, since its handles
mean nothing in the new game, and asks for it again: the server has the owner
hand it over, to that player alone, as for a late joiner. The owner's game
that starts over, or whose mission script is gone from the engine's list
without ending, reports the mission failed.

---

## 12. Rewards, death and failure, as decided

### 12.1 Everybody is paid

The mission's `ADD_SCORE` on the owner (89 sites) is replayed to every
participant, whose own engine pays its own player. Under the server's
`money = shared` there's only one wallet, so there it's paid once. A
server with `missionPayHelpers = false` pays the owner alone: it does not
relay the reward (`protocol.md` §1.46).

What a mission *charges* is the owner's alone (decided 2026-09-25): Bomb Da
Base: Act II takes $100,000 and The Exchange $500,000 (`ADD_SCORE` with a
negative amount), and only the owner's balance was ever checked, so a helper
would have gone into debt. The owner's machine does not send an `ADD_SCORE`
that takes money, and a participant drops one that comes from an older
owner.

What the story does to the player's wanted level is each player's own where
the mission saves and gives it back: the RC, 4x4 and Mayhem runs and two of
the Yardies' missions store the level in a global at the start (`01C0`) and
restore it from there at the end (`010D`). Every participant stores its own
level in that same global, and gets its own back.

The stats the pass registers (`REGISTER_MISSION_PASSED`,
`PLAYER_MADE_PROGRESS`) are part of the campaign delta (§5.5), so every
machine's percentage and every save counts the mission.

**As built** (2026-09-25): they, and the three islands' `*_PASSED` (Last
Requests, Love's third mission, The Exchange, two of which play the
radio's "island open" announcement), go in the delta *only*, and aren't run
live. A participant applies the delta once the mission is over, the way a
late joiner does, and the owner skips its own. Run live as well, a
participant would count each of them twice.

### 12.2 A death fails the mission

`missionFailOnDeath` already sits in `ServerConfig`, default `true`. It's the
switch for this.

- **The owner dies or is busted:** the engine fails the mission, exactly as
  in single player.
- **A participant dies or is busted:** with the switch on, the session tells
  the owner's machine, and the owner's script fails the same way, through the
  engine's own `DoDeatharrestCheck` path (§5.7). With it off, the participant
  goes to the hospital and back into the fight.
- **Either way, everybody sees MISSION FAILED,** because that's a replayed
  `PRINT_BIG`.

The server orders it once per mission. An owner's script caught between the
gosubs of its top level when the order comes can't be unwound yet, so the
owner's machine keeps the order and tries every frame until it can, or the
mission is over (built 2026-09-25; it used to be dropped, and the mission
went on as though nobody had died). Two missions turn the death check off
for their whole length and watch their player themselves: TAXI and HOOD1
(Uzi Money). Neither would ever see a participant die, and neither has a
failure branch: the gosub after its body is its cleanup. So both are unwound
all the same, and end the way they end a shift or a frenzy.

### 12.3 The owner leaves

A mission's state is one machine's script memory, and it can't move to
another machine. So when the owner disconnects, the mission fails for
everybody, and the next player to walk into the marker starts it again. The
same goes for the owner's game crashing, which the server now notices in
about five seconds rather than up to thirty (`protocol.md` §2.2); until then
everybody's mission waited on a game that was gone.

**A dropped connection is not a quit** (built 2026-09-25). The owner's game
is still running the mission, its script alive and everything it made still
standing. It used to finish it alone: nothing it showed reached anybody, and
its campaign delta was refused as somebody else's. Now it keeps the mission
as the session's while the connection is down and, back, offers it to the
session again (`C_MissionStarted`); the server takes it up as it takes any
start it never granted, and the owner hands every participant what it has up,
as for a joiner (§11.5). If somebody else started a mission meanwhile, the
old one stays that game's own.

---

## 13. Everything else a whole playthrough needs

The list of things that would break a full playthrough if nobody did them,
found while measuring the rest.

| What | Why it breaks | What to do |
|---|---|---|
| The clock and the sky | The session host owns them (`protocol.md` §2.7), but INTRO, EIGHT and CAT1 set the time and six missions force the weather. The owner's clock would be dragged back to the host's. | Built (§15): for the length of a mission, the mission owner is the session's clock and weather host (`sdk/include/coopiii/sky.h`). Nothing new travels: the server and every client work it out from S_MissionState. |
| Empty streets | Missions call `CLEAR_AREA` (205 sites in 37 missions) and set the ped and car density on the owner. Every other machine keeps hosting its own traffic, which is shared, so it stays on everybody's screen, in the middle of cutscenes and where the mission parks its cars. | Built (§15, "the streets"): both are replayed to every machine, and each clears and thins what it hosts. |
| The islands | Only one island's collision is in memory (`roadmap.md` §2.2). | The start gate, the checkpoints and the moves keep a mission's players together. Outside missions it stays the known limit. |
| Getting in | A joiner has to reach the same campaign state. | Load the host's save at join (§5.5), then replay the server's campaign log. A joiner during a mission watches, or waits for it to end. |
| Saving | Every machine has the same campaign, so every save does. | Anybody saves at their own safehouse, outside missions (§11.4). |
| Retrying | A failed mission starts over from its marker, as in single player. | End the session's mission with its script, not at its first `MISSION_HAS_FINISHED`, and clear the failed try's empty cars from every screen (§15, failing and trying again). |
| Special missions | RC Toyz has one RC car. Turismo and the 4x4 runs are races. Sayonara Salvatore is a sniper mission. | The owner drives the toy and the others watch. A race is the owner's to win, and the helpers can block the rivals (§5.6). A sniper mission works as it is. |

---

## 14. The plan, updated for the decisions

Phase 0 and Phase 1 stay as §7 describes them. Then:

**Phase 2: a shared campaign, together.** Everything in §7's Phase 2, plus:

- the start gates and the checkpoints for everybody (§5.6); the host's kick
  for somebody who never comes is built (§9);
- moving everybody after each `SET_PLAYER_COORDINATES` (§11.2);
- the HUD widgets' values (`mission-audit.md` R16);
- cutscenes for everybody, with the loading hold (§11.3);
- everybody paid (§12.1);
- `missionFailOnDeath` read at last (§12.2);
- the owner's clock and weather during a mission (built, §15), and the
  replayed `CLEAR_AREA` and densities (§13).

Done when two players play Luigi's first three missions from the marker,
watch the cutscenes together, both get paid, and one of them dying fails the
mission for both.

**Phase 3: everybody in the fight**, as in §7.

**Phase 3.5: difficulty.** Tougher enemies first, then more of them, behind
`missionEnemies` (§10). It's cheap once mission entities are sync'd, and it's
worth having before anybody plays Sayonara Salvatore with four people.

**Then the whole game, mission by mission.** Play through in order and fix
what each mission shows up. The work is per opcode, so each fix carries to
every later mission that uses it. §4.3's overrides are there for the few
missions that still don't play well after that.

[mission-audit.md](mission-audit.md) §4 orders the sixteen requirements the
audit found by how many missions each opens up.

---

## 15. What is built

> **2026-09-24.** Behind `missions = on` in every player's CoopIII.ini (on by
> default since). The script engine's addresses were III.CLEO's and
> plugin-sdk's; they have been read out of the exe since and are in
> `addresses.h` (`addresses-unverified.md`, "The script engine", has what
> was wrong). Without it every mission is each machine's own, as before.

**The session's one mission** (`protocol.md` §1.29):

- The server holds one mission at a time (`server/core/missionslot.h`). A
  claim at a start gate is granted once every other player is inside the
  start's area or within `missionMargin` of it (5 m, CoopIII-Server.ini).
  Until then everybody's log says who is missing, the missing ones'
  included. None of the session's mission status goes in the chat.
- Everybody connected is in the running mission, a joiner too, but for a
  game in a mission of its own until it is out (§11.6), and every
  participant's `$ONMISSION` is held at 1 until it ends. So nobody else can
  start a mission, save or pick up a rampage meanwhile, exactly as vanilla
  refuses them on a mission.
- `missionFailOnDeath` is read at last: a participant dying or busted fails
  the owner's mission through the engine's own `DoDeatharrestCheck` path, so
  the mission prints its own MISSION FAILED and cleans up as for the owner's
  death. The owner leaving fails it for everybody.
- The host kicks from the chat, for somebody who never comes (§9).

**On the owner's machine** (`client/src/game/mission.cpp`):

- The 66 contact and payphone launches are held at `CAN_PLAYER_START_MISSION`
  until the session grants them. The trigger is held at the question while
  its player stays in the marker, so the mission starts the moment the last
  player arrives. The three save points ask the same question and are told
  apart by what follows it. Only the host's are claimed: on any other
  machine they are answered no, and nothing is claimed (§5.8).
- The launches that never ask it, the odd jobs' button, the RC van's spot,
  the 4x4 and Mayhem cars, are held at `START_MISSION` itself until everybody
  is at the RC van's spot or beside the owner. Getting out of the car, or
  driving off the RC spot, gives the start up, and the trigger goes round its
  loop as it would have.
- `START_MISSION`, `MISSION_HAS_FINISHED` and `REGISTER_MISSION_PASSED`
  report the start, the end and whether it was passed.
- A location check of the mission whose area holds one of its coordinate
  blips waits for every participant (C1). The owner's machine decides it from
  everybody's snapshots, and the others read who is still missing. It waits
  60 s at most (`MISSION_CHECKPOINT_WAIT_MS`), then goes on without them, so
  a timed mission is never lost with its owner standing in the checkpoint.
  A mission with a countdown up, a race, an odd job and the RC, 4x4 and
  Mayhem runs don't wait at all, and bring along whoever is more than 150 m
  behind for 10 s instead (§5.6). The wait, whether races and timed
  missions wait too, and the two distances are the server's
  (`missionCheckpointWait`, `missionTimedCheckpoints`, `missionCatchUp`,
  `missionFallBehind`, `protocol.md` §1.46); these are their defaults.
- Who a start or a checkpoint waits for, and for how long more, is a small
  line in the HUD's bottom-right corner on every machine it concerns, in the
  version mark's face: "Waiting for carol - 42 s", "alice is waiting for you
  at the start". The log still has the rest, and the chat none of it.
- Somebody who joins or reconnects in the middle of the mission, comes out of
  their own intro into it, or comes back from the hospital with the death
  rule off, is brought beside the owner (§11.5).

**What the mission shows** (`client/src/game/replay.h`, `protocol.md` §1.29):
the words on the screen (the queued big ones, the Vigilante's bonus, and
Salvatore's leaving time too), Payday For Ray's phone messages, the title
the trigger prints and the make-safe before it, the pay (everybody is
paid, §12.1, except under a shared wallet), `INCREMENT_MISSION_ATTEMPTS`,
`REGISTER_MISSION_PASSED`, `PLAYER_MADE_PROGRESS` and the islands done (the
last three by the campaign delta alone, §12.1), the odd jobs' own stats (fares, taxi takings, lives saved,
criminals caught, fires put out, the paramedic's level, the 4x4 and Mayhem
records), the passed tune, the coordinate blips, the HUD's timers and
counters with their values (R16), and what the world keeps of it (§3.4: the
parked-car generators, the building swaps and hidden objects, the roads, the
garages' types). Each is replayed through the participant's own interpreter,
with the blips translated.

**The streets, the lines and what is drawn** (mission-audit.md R15,
`protocol.md` §1.29, `client/src/game/missionworld.h`): `CLEAR_AREA` and the
two density multipliers are replayed, so every machine clears and thins the
traffic it hosts itself, and nobody else's drives through the owner's
cutscene. `SET_ZONE_CAR_INFO`, `SET_ZONE_PED_INFO` and `SET_GANG_WEAPONS` are
replayed and kept in the campaign delta (a save keeps the zones and the
gangs), so every machine's own traffic is the RC missions' Diablos and
Mafia cars, and Triads And Tribulations thins the Triads everywhere. A
participant keeps what its streets were before the mission changed them: the
densities go back when the mission ends whatever happened, the zones and the
gangs only when the mission's own cleanup never reached it (the owner left,
or the connection did). The mission's lines of dialogue are loaded, placed
and played on every machine, each played only once that machine's copy is
in, in the ear or at the place the mission gives (Chico, Phil's bunker); the
pager, the fire truck's "burning vehicle reported in" line, Arms Shortage's
attackers' blips, Luigi's second mission's cylinders, `LOAD_SCENE` before a
move, The Exchange's credits and Give Me Liberty's critical restart (every
participant starts over beside the bridge, or the hideout) go to everybody.
The coronas the 4x4 runs and the Mayhem draw every frame go the way the blue
markers do: the owner's machine says when each is lit, where, and when it is
out, and each participant registers it every frame. Chaperone's club light and the
shadow on its floor (`DRAW_LIGHT` and `DRAW_SHADOW`, drawn every frame
like a corona) go the same way, and each participant runs the instruction
itself every frame it is up. Whatever of it is still up when the mission
ends, a line, the credits, a sphere, a corona, a light, is taken down. The
camera shaking, the particles, the end of the game's tune and the
continuous sounds (Frank's party, 8-Ball's fire), whose handle each machine
keeps in its own global, go to everybody too. So do the settings a mission
holds for a while, and each is put back at the end: free resprays, crime
sensitivity, the restart overrides (kept for a participant already on the
way to that restart), the world held still for a cutscene, every car unhurt,
traffic round the camera, the near clip and the music through a fade. A
sound still playing is taken off only when the mission's own cleanup never
reached this machine.

**Fewer packets** (`client/src/game/effectshape.h`): what a mission's script
runs every frame no longer goes out every frame. An instruction that sets
something is dropped when it says what was said last, and said again once a
second at most; a changing one goes ten times a second at most, the newest
kept until its turn, and all of it at the mission's end. The words on the
screen are said again only before they would run out. A blip taken off and
put back in the same frame, S.A.M.'s plane, stays one blip under the owner's
first handle for it, and moves at most twice a second. Nothing that makes,
takes away, pays or happens once is ever held back.

**The cutscenes and the moves** (§11.2, §11.3): the models a cutscene loads,
the cutscene itself with its objects and heads (translated like the blips),
the fades, the widescreen bars, the fixed camera and the frozen controls
reach everybody, and the other players are not drawn while it plays, nor
while the mission's widescreen bars are up for a scene it frames itself
(8-Ball's walk, Chaperone's club). Meanwhile a helper's player, frozen
where the scene found it, can't be hurt. A camera the mission points at
the owner's own player (`CAMERA_ON_PED` on the ped `GET_PLAYER_CHAR` made,
Arms Shortage's run to Phil's bunker) points at each participant's own. The
moves such a scene gives the owner's player as a pedestrian (8-Ball's walk,
Arms Shortage's run) stay the owner's, since each helper stands hidden from
the others for it. The
owner's mission waits in its own model-loading loop until everybody's models
are in, 5 s at most, so every machine's scene starts together, and only the
owner can skip it. The
mission's `SET_PLAYER_COORDINATES` moves every participant to a spot of its
own round the owner's, clear of the buildings: 3 m out on foot and 7 m in a
car, whose car the engine moves with them, with a place on the ring for each
of the mission's participants and nobody else (it used to be a step and a
half, for everybody connected, which put cars into each other). Whatever of
all that is still on when the mission ends, the owner gone mid-scene
included, is put back.

**The last range of instructions, 1100..1154** (`addresses.h`, above
`CRunningScript__ProcessCommands1100To1199`): its handler is `0x00589D00`,
the dispatcher's last link, and until now it was not hooked at all, so
nothing in it reached anybody. It is now, with the others. A move of a
participant to a place on another island (`SET_PLAYER_COORDINATES`,
`WARP_PLAYER_FROM_CAR_TO_COORD`, `RESTART_CRITICAL_MISSION`) loads that
island first through the engine's own `LOAD_COLLISION_WITH_SCREEN`, the
island worked out the way the rampage vote's move works it out, so Last
Requests' helpers land in Staunton with Staunton under them. The owner's
own `LOAD_COLLISION_WITH_SCREEN` (Last Requests, S.A.M. near the platform)
runs on a participant only while that participant stands on that island
or between islands, never under one standing on another. Lips' car made
stronger (Mike Lips Last Lunch) is on every copy, since a helper may drive
it; Bait's cartel car is sent back after the player where it is simulated;
The Exchange's end-of-game music is loaded for everybody's END cutscene.

**The clock and the sky** (§13, `sdk/include/coopiii/sky.h`): while the
session's mission runs, its owner's game is the session's clock and weather,
not the host's. The owner takes the sky from its own `START_MISSION`, before
the server has answered, so the host's next world packet can't put the
mission's `SET_TIME_OF_DAY` back; the server takes `C_WorldState` from the
owner alone until the mission ends, and a sky cheat goes there too. The owner
reports a jump of its clock or a turn of its weather at once rather than on
the next 1 Hz beat. The host follows like anybody else, and when the mission
ends it goes on from the clock and sky the mission left, its weather turning
on its own again: no jump back. None of `SET_TIME_OF_DAY` and the three
weather instructions is on the replay list, so nothing but the world packet
moves a participant's sky. `ADD_PARTICLE_EFFECT` is: Give Me Liberty's
burning police cars at the bridge were on the owner's screen alone.

**What the mission leaves behind** (`protocol.md` §1.30): as it ends, the
owner sends every global it changed, the main-script threads it started and
the world instructions above. A global counts when the mission assigned it a
number, did arithmetic into it (with a literal or another variable, timed or
not), converted into it or copied a number into it: `$RC1_RECORD =
$COUNTER_RC` and the 4x4 and Mayhem best times, so the next owner's "new
record" and its reward start from the real record. A global holding a
handle is left out: one a pedestrian, car, object, blip, pickup, sphere or
fire was made into, and a copy of one, or of a global the mission never
wrote, since that may be any of the main script's handles. The server numbers them
and keeps a log; every machine keeps its own copy and applies, in order, the
part its game lacks, worked out from its own globals. So the next mission's
trigger runs everywhere, a passed mission is closed everywhere, a late joiner
or a machine that loads a save during the session catches up, and nothing is
started twice. A machine with a different main.scm applies nothing, and says
so.

**Tested:** `sessiontest` (the slot), `servertest` (a real server and real
clients over loopback, the whole flow, the effects' relay to everybody and to
one player, the campaign log), `clienttest` (`MissionSync`, the campaign log
against a stub game's globals, loads and new games; the gate classification,
the areas, the checkpoint test, the unwinding, the replay encoding, what the
mission has up and the delta's parts, which are pure; the streets, the
lines, the spheres, the coronas and the shaping, `missionworld.cpp`).
With a retail exe, `COOPIII_GTA3_EXE=<path> clienttest` checks the script
engine's addresses. Not run in game.

**The mission's pedestrians and cars** (§5.3, `population.cpp`): whatever
the owner's mission puts into the world while one of its instructions runs is
hosted by the owner's machine like its traffic and its crowd, and announced
with `AMBIENT_MISSION`. So its enemies, its targets and their cars stand on
every participant's screen, where they are shot, rammed and killed through the
population machinery, and their hits on a participant reach that participant.
A participant's hit on one the mission made only-the-player-can-hurt counts as
the player's (R1). An enemy the mission tells to kill the player goes for
whichever player is nearest it, the CHAR form of the same objective at that
player's replica. A blip or the camera on one of them reaches everybody, by
its netId, and lands on each participant's replica.

**Enemies for more than one player** (§10): under `missionEnemies =
tougher` each enemy's health and armour grow with the players in the mission,
and under `more` it also gets a copy for each player past the first, which
the mission counts as the same enemy: it is dead once every copy is.

**What the story does to the player** (R12) happens to every participant's:
the weapons the mission hands over and takes away, the ammo, the weapon in
hand, the wanted level and its minimum, health, being seen, being ignored by
the police and by everybody, and where a death or an arrest wakes
everybody up. `WARP_PLAYER_FROM_CAR_TO_COORD` moves everybody the way
`SET_PLAYER_COORDINATES` does. Whatever of it is still on at the end is put
back, except the paramedic's reward: never getting tired is kept, by every
participant and in the campaign delta, as a save keeps it in single player.

**The contacts' markers** (`protocol.md` §1.30): the icon and sphere of the
next contact are made inside the mission that unlocks them, into a main-script
global (`$JOEY_MISSION_MARKER` in Drive Misty For Me, Toni's in Cipriani's Chauffeur,
Salvatore's in Salvatore's Called A Meeting, which takes the other three
off), and each machine makes its own into its own global. Taking one off,
or changing it, by that global reaches every machine too, when the blip is
a live one the mission did not make. Both are in the campaign delta, so a
late joiner and a save get the markers right. The phones' and the
safehouses' markers are the main script's own threads on every machine and
need nothing.

**Whose contacts are drawn** (§5.8, `protocol.md` §1.49): the host's. A
machine that is not the host leaves its own contacts out of the radar's
icons and the street's markers, for the length of each draw and no longer,
and draws the host's, which the host's machine reads off its radar twice a
second and sends when they change; its own story and payphone starts are
answered no. Out of the session its own are drawn and started again. The
safehouses, the bomb shops, the Pay'n'Sprays and Ammu-Nation stay every
machine's own. Covered by `clienttest` (the rule, the list, which blips are
contacts, and both draws against gta3.exe) and `servertest` (the relay). Not
run in game.

**The mission's explosions and fires** (R8) go off at their place on every
machine, where each hurts what that machine decides the damage of: its own
player and its own entities. A script fire is translated like a blip, and
whatever of them is still burning when the mission ends is put out.

**Everybody has to be quiet** (R13): `HAS_CHAR_SPOTTED_PLAYER` is true when
the pedestrian can see any participant (ahead of it, nearer than 40 m, no
building between, as re3's `OurPedCanSeeThisOne`), and
`IS_PLAYER_SHOOTING_IN_AREA` when any participant's own snapshot says they are
firing inside the area. In Cutting The Grass the Spookometer's locates against
Curly Bob, and the Mafia-car test after them, are answered for whichever
player is nearest him. Each is a single condition, only ever widened to true.
Deal Steal's and Plaster Blaster's checks are still the owner's. A pedestrian
of the mission that fears the player (`SET_CHAR_THREAT_SEARCH`'s
`THREAT_PLAYER1`) sees a participant as the player too: when the engine's
own `CPed::ScanForThreats` finds nobody nearer, a participant it can see
within 60 m is its threat, and it goes for them as it would for the player.

**The mission's objects** (R3): an object instruction names its object by
the global that holds it, and goes as that global, so every machine works on
its own: the main script's doors that every machine made at startup, and the
objects the mission makes, which each machine makes into the same global.
Made, moved (`SLIDE_OBJECT` and `ROTATE_OBJECT` every frame the mission runs
them), turned, pushed, blipped, let go of and taken away. A participant runs
one only on a live object of its own, and lets go of what the mission made
there when it ends. A break a participant's car does to one breaks the
owner's copy too, and everybody else's, so the owner's
`HAS_OBJECT_BEEN_DAMAGED` counts the stalls a participant smashes.

**The mission's pickups** (R2): the pickups it lays out are made on every
machine too, and the pickup sync names them by where they are, as it names
every script pickup. Its weapons, armour, health and cash are everybody's own
(`PICKUP_F_STASH`): each player takes theirs, the first one taken moves the
mission on, and the mission taking one away is put off until it ends, so
nobody's copy goes before they have had it. A briefcase or a package stays one
pickup, whoever gets there first. A Drop In The Ocean's floating packages,
which the pickup sync cannot name by where they are, are made where the
owner's plane drops them on every machine, and a package anybody's boat takes
counts for the owner's mission and goes from everybody else's water.
What a mission puts up for good is not its stash: a shop's gun or a pickup on
the street that comes back after it is taken (Cipriani's Chauffeur's Uzi at
Ammu-Nation, Phil's M16, shotgun and rocket launcher, and his armour when
Arms Shortage is passed) is made into its global on every machine and stays
when the mission ends, and the out-of-stock Uzi it takes down goes from every
machine too. Both are in the campaign delta, so a late joiner and a save
have the shop (`protocol.md` §1.30).

**The seats the mission's passengers need** (§11.4, R4): while one of the
mission's pedestrians is on the way into a session car as a passenger, Misty
or a fare or somebody following the player, nobody else's seat key sits them
down in it, and when the free seats there are fewer than the pedestrians
heading for them, as many of the players riding in it get out as it takes,
the last seated first. The Paramedic's "Ambulance full!!" counts only the
patients (`GET_NUMBER_OF_PASSENGERS` without the players riding in it).
Since 2026-09-30 that includes the ones the engine gave up on: an order into
a car that a player's copy filled, which the engine drops in the frame it is
given, and a follower of the owner who stays on foot because his car is full.
The dropped order is given again once the seat is free, so Drive Misty For Me
no longer waits for ever with a guest beside the owner (mission-audit.md R4,
"Corrected").

**Any participant in the mission's car** (mission-audit.md R4b): the owner's
`IS_PLAYER_IN_CAR` is yes for any participant the session seats in that car,
driving or riding, in every mission, so a helper can take Lips' car while
the owner drives his own. Not in an `if and` that asked where the owner is
first. `IS_PLAYER_IN_ANY_CAR`, `IS_PLAYER_IN_MODEL` and
`STORE_CAR_PLAYER_IS_IN` stay the owner's; the checkpoints were already
everybody's, in whatever car.

**A respray of a car somebody rides in** (`game/garage.h`,
`PassengerKeepsPaint`): the Pay'n'Spray acts on `FindPlayerVehicle()`, which
a passenger's machine answers with the car he rides in, so both machines
resprayed it, each its own colour, and only the driver's said so. A
passenger's own respray now keeps the colours the car went in with, and the
driver's `C_Respray` paints it on every screen.

**A passenger can always get out** (`game/passexit.h`, `TickPassengerExit`):
the exit key gives up for a car locked with its player inside, which a
mission's `LOCK_CAR_DOORS` reaches every copy with, and for anybody whose
controls the owner's scene has taken, which is every participant. A guest
riding in the bomb car was kept in until it went up. A player riding beside
somebody else who presses the exit key and is still in his seat 400 ms later,
in a car slow enough for the engine's own exit, is let out beside it the way
the seat key does it. Not the driver, not a boat or a bus, not in a cutscene,
not while the chat line or the menu has the key.

**The car the mission puts its player in** (`protocol.md` §1.40): Last
Requests' Reefer, Chaperone's Stretch, Cipriani's Chauffeur's car. Once the
session names it, the owner's machine hands its free passenger seats to the
participants on foot, one each, nearest first, keeping one back for each of
the mission's pedestrians on the way to it, and each is put straight in. Two
never take the same seat. Whoever gets none stays where he is and follows in a
car of his own. A boat has one passenger seat, so while the owner is on the
water his checkpoints stop waiting for anybody who has no boat.

**Somebody who comes in late** (§11.5): a player who joins while the
mission runs, or comes back from a dropped connection, is handed what it has
up, to them alone: its blips and the changes to them, the pickups nobody has
taken and the stash, its fires, its objects where they are now, and the HUD's
timer and counter, whose values follow at once. Each is named as the session
names things then, and their machine skips whatever reached it anyway.

**The mission Cessnas** (mission-audit.md R7): S.A.M.'s plane and A Drop In
The Ocean's fly on every machine, and a helper's rocket brings the mission's
down. The Catalina helicopter, the RC buggy, the power pills and the crusher
crane are listed there with what a game has to show about each.

**A helper's garage** (mission-audit.md R5): a mission garage takes the car and
a Pay'n'Spray resprays it on whichever machine's player brings it, and the
owner's mission hears the helper's garage.

**A car's bomb** (mission-audit.md R6): the bomb a car carries is the same on
every copy of it, whoever's bomb shop fitted it or whoever set it ticking, so
a helper can take Lips' car to 8-Ball's. It goes off on the machine that
simulates the car.

**The mission's word to a car** (mission-audit.md R9): moving a car, turning
it or setting its health reaches the machine that simulates it, when a helper
drives it or settles it, and a car's locked doors, its colour and the brakes
the mission puts on the player's car reach everybody.

**Everybody's kills** (mission-audit.md R11): a pedestrian any participant
kills counts for the owner's mission, whichever machine hosted it, so Uzi
Rider's and Bait's counts are the group's.

**A game of its own** (§11.6): a game in its own intro or info scene is
nowhere to the session until it is out, so the first mission waits at the
bridge for everybody's intro to end. A participant's game that loads or
starts over mid-mission is handed what the mission has up again, and the
owner's fails the mission.

**Failing and trying again** (a test run on 2026-09-24, Give Me Liberty
failed and retried three times over). A failure runs `MISSION_HAS_FINISHED`
twice, once in the failure and once in the cleanup after the critical
restart has put its player back at the bridge, and the session's mission
used to end at the first. So the owner's machine was "in a mission of its
own" for the four seconds of its restart, a retry could be claimed meanwhile,
and nothing of the cleanup reached anybody: its blips, its deletions, its
smoke put out, and the flags it puts back, which the campaign delta had
already sent as they stood. Now:

- the mission ends when its script does, and everything up to then is the
  mission's, sent as it runs;
- a failed mission's cars that nobody sits in go from every screen, the
  session's by the server and the rest by the owner's own engine, so the
  retry's Kuruma is not parked beside the last one's wreck;
- the objects the mission keeps past its end (`DONT_REMOVE_OBJECT`, the
  wrecked police cars on the bridge, which main.scm makes once behind a flag
  the campaign keeps) are kept on every machine, where each participant used
  to let go of its copy at the end and never had it again;
- the smoke and flames the mission lit are put out on every machine
  (`REMOVE_PARTICLE_EFFECTS_IN_AREA`), so a retry does not light a second set;
- nobody lying dead on the marker is at the start: a respawn resets the
  camera and the controls and clears the streets, which in the first seconds
  of a mission is its cutscene gone;
- "Hey! Get back in the vehicle!" is for whoever got out of the mission's
  car: the owner's own stays on the owner's screen, and a participant who
  gets out of the car the mission asks `IS_PLAYER_IN_CAR` about is told
  alone, by the owner's machine.

**Next, in this order:**

1. **More games.** Most of the above has only been through the test suites
   and short sessions. What matters is whether the owner's mission, its
   effects and its campaign delta do what the tests say they do on two real
   machines, mission after mission (§5).
2. Deal Steal's and Plaster Blaster's stealth checks (R13). Their scripts are
   not in the converted source this was measured on, so which instructions
   decide "you have been spotted" has to be read off the retail main.scm
   first.
3. The owner-drives rule's four requirements are built (mission-audit.md
   C2: R4, R5, R6, R9). Until a game has shown them, the owner at the wheel
   is the safe way.
4. §5.5's checks: the host's save over the wire for a joiner who has none,
   and the campaign globals compared across machines. The second needs a set
   of globals every machine's own scripts leave alone once a delta has set
   them, or it would cry wolf.

**What only the game can settle**, in the order worth doing it:

1. `COOPIII_GTA3_EXE=<path to a 1.0 gta3.exe> clienttest` checks the script
   engine's addresses against the image ("the script engine's addresses
   against gta3.exe").
2. Done statically on 2026-09-24: `DoSettingsBeforeStartingAGame`, the bomb
   bits, `APPLY_BRAKES_TO_PLAYERS_CAR`, the garages' and the Cessnas'
   questions all held. `IS_CAR_IN_MISSION_GARAGE` was `03D4` and is `021C`,
   `OVERRIDE_NEXT_RESTART` was `016C` and is `016E`. `CTimer::m_FrameCounter`
   going back is not a load's witness: a load reads it from the save, and the
   end of an instant replay puts it back (`addresses-unverified.md`,
   REFUTED).
3. Two machines, a server, and in each case the log's `missions:` and
   `newgame:` lines:
   - the lobby: both launchers in, the host starts a new game, both games go
     past the menu by themselves, and Give Me Liberty waits at the bridge
     until both intros are over (§11.6);
   - a participant loading a save in the middle of a mission is handed it
     again, and the owner doing it fails the mission for both;
   - Mike Lips Last Lunch with the helper driving Lips' car into 8-Ball's;
   - Uzi Rider with the helper's kills;
   - a helper driving the owner in a mission car the mission moves or brakes;
   - a helper delivering a car to a lock-up (Gangcar Round-Up, Grand Theft
     Auto) and taking one through a Pay'n'Spray;
   - S.A.M. with the helper's rocket, and what the others see of the
     Catalina helicopter in The Exchange and of the RC buggy
     (mission-audit.md R7 says what each answer means);
   - three or four players: a checkpoint's corner line counting down, and the
     mission going on at 0 s with somebody still away; a third player joining
     in the middle and landing beside the owner, on another island too; a
     helper dying with `missionFailOnDeath = off` and coming back from the
     hospital to the owner; the owner pulling their network cable for ten
     seconds or more, and the mission carrying on for everybody once they
     are back; the owner's game killed, and everybody's mission over within
     about five seconds;
     four players moved by a mission's `SET_PLAYER_COORDINATES` in cars, with
     none of the cars touching.
4. `mission-audit.md` §5, what only a game can settle.

**To try it:** `missions = on` in every player's CoopIII.ini, a server of this
build, two players. One stands in Luigi's marker. The HUD's corner and
CoopIII.log say who the start waits for, and the mission starts when the
other arrives. Then either one's
death should fail it for both. The client's log says `missions: on`, or
which range handler it refused and why.

