# Co-op campaign: how the mission script survives having 8 players

The goal is the GTA III campaign, played together, with the game otherwise 1:1
faithful. This document is about the one thing that makes that hard.

Every engine claim is cited as `file:line` in `reference/re3` (branch `main`).

---

## 1. The actual problem

GTA III's campaign is a single compiled SCM program. It was written for exactly
one player, and the engine agrees: `NUMPLAYERS = 1` (`src/core/config.h:7`,
see `protocol.md` §1.3). Everything the script asks about "the player" resolves
to `CWorld::Players[0]`.

So the problem is not "how do we send mission state over the network". It is:

> The script must keep believing there is one player, while N players
> experience the mission.

Any design that makes the script aware of 8 players is a rewrite of the
campaign, and a rewritten campaign is not 1:1 with anything.

### 1.1 What the script actually is

| Thing | Where | Size / limit |
|---|---|---|
| Script bytecode + **all global variables** | `CTheScripts::ScriptSpace` (`Script.h:429`) | 160 KB (`128 KB` main + `32 KB` mission, `Script.h:406-409`) |
| Running scripts | `CTheScripts::ScriptsArray` (`Script.h:430`) | 128 (`Script.h:412`) |
| Active list, walked each frame | `pActiveScripts` (`Script.cpp:1995-2000`) | linked list |
| Mission-owned entities | `CTheScripts::MissionCleanUp` (`Script.h:445`) | released when the mission ends |
| "A mission is running" | `bAlreadyRunningAMissionScript` (`Script.h:450`) | **one mission at a time** |

The detail that matters is that the globals live *inside* `ScriptSpace`. Mission
progress is not a struct we can serialize field by field, it is offsets into a
160 KB blob whose meaning is defined by the compiled SCM.

### 1.2 Mission lifecycle

```cpp
case COMMAND_LAUNCH_MISSION:                       // Script.cpp:3784
    CollectParameters(&m_nIp, 1);
    CRunningScript* pNew = CTheScripts::StartNewScript(ScriptParams[0]);
    pNew->m_bIsMissionScript = true;

case COMMAND_TERMINATE_THIS_SCRIPT:                // Script.cpp:2649
    if (m_bMissionFlag)
        CTheScripts::bAlreadyRunningAMissionScript = false;
```

Per frame, `CTheScripts::Process()` (`Script.cpp:1918`) walks the active list
and calls `CRunningScript::Process()` (`Script.cpp:2023`), which runs commands
until one yields:

```cpp
while (!ProcessOneCommand())                       // Script.cpp:2033
    ;
```

`ProcessOneCommand()` (`Script.cpp:2050`) dispatches by opcode range into
`ProcessCommands0To99` … `ProcessCommands1000To1099`.

---

## 2. The design: one script, on the host

Only the host runs `CTheScripts::Process()`. Clients suppress theirs entirely.

It is the load-bearing decision here, and the reason is determinism rather than
bandwidth. Two machines running the same SCM against different player positions
diverge within seconds, and `protocol.md` §2.1 already established there is no
rollback available to us: GTA III physics is frame-rate-coupled through
`ms_fTimeStep` and not reproducible across machines. A campaign that desyncs
silently and cannot resync is worse than one only the host drives.

So the script exists in exactly one place, and everyone else receives its
effects.

### 2.1 What "the player" means to the host's script

The host's own ped. The script reads its position, health, wanted level and
vehicle, and none of that changes. As far as the script can tell, nothing
unusual is going on.

Clients are, narratively, along for the ride. They can shoot, drive, die and
help, and the mission entities are real for them, but the *conditions* are
evaluated against the host until Tier 3 (§4).

### 2.2 What has to go over the wire

Most of it is already built. Mission-spawned peds and cars are ordinary netid
entities (`protocol.md` §1.5, §1.6): the host spawns them because the host's
script spawned them, and they replicate like any other entity. No new
machinery.

The new part is the presentation layer, since it is script-driven and clients
have no script:

| Effect | Engine side | Packet |
|---|---|---|
| Subtitles / mission text | `CTheScripts::IntroTextLines` (`Script.h:433`) | `E_SCRIPT_TEXT` |
| Radar blips / markers | `CRadar` | `E_BLIP_ADD` / `E_BLIP_REMOVE` |
| Script spheres (the blue markers) | `ScriptSphereArray` (`Script.h:435`) | `E_SPHERE_ADD` / `E_SPHERE_REMOVE` |
| Mission start / pass / fail | `m_bIsMissionScript`, `FailCurrentMission` (`Script.h:453`) | `E_MISSION_STATE` |
| Cutscene enter / exit | n/a | `E_CUTSCENE` |
| Screen fades | n/a | folded into `E_CUTSCENE` |
| Mission entity cleanup | `CMissionCleanup::Process` (`Script.h:390`) | existing despawn |

These are all low-rate reliable events (channel 1), not snapshots.

### 2.3 What deliberately stops being 1:1

Three places where faithful single-player behaviour is wrong for co-op. Each one
is a server option that defaults to the co-op-friendly choice, so a group that
wants the strict experience can still have it:

1. **Death fails the mission.** In SP, the player dying ends the mission
   (`DoDeatharrestCheck`, `Script.cpp:2029`). In co-op that should not happen
   while someone is still alive. Option: `MissionFailsOnAnyDeath` (default
   off). With it off, only the host dying fails, and dead clients respawn.
2. **Cutscenes freeze the player.** `MakePlayerSafe` freezes the local player.
   Remote players must be frozen too, or they wander around during cutscenes
   and through the scripted camera. `E_CUTSCENE` carries this.
3. **One save, host-owned.** Campaign progress lives in the host's
   `ScriptSpace`. Clients do not save campaign progress from a co-op session;
   the host's save is the session's save.

None of these change how a mission plays. They change what happens when
multiplayer creates a situation the SCM was never written to see.

---

## 3. Why not the alternatives

**Run the script on every machine, keep it in sync.** Would need every
input the script reads to be identical everywhere, including physics results
and RNG. §2 covers why that is not available. It also fails silently.

**Serialize `ScriptSpace` and broadcast it.** 160 KB of opaque blob whose
layout is defined by the compiled SCM. Even diffed, a client applying it would
have script state with no running script to interpret it. The globals only mean
something to the code that reads them.

**Rewrite the campaign as a multiplayer-aware script.** The honest "real"
solution, and also a different project: it stops being GTA III's campaign.
Rejected, since fidelity is the goal.

---

## 4. Tiers, and what ships when

The design above is not all-or-nothing. It arrives in three steps, each
playable:

### Tier 1: free roam (v1, current scope)

No mission sync at all. Clients suppress `CTheScripts::Process()`, so the
campaign does not run; players share a living Liberty City and do what they
like in it. This is what `protocol.md` §3 scopes, and the existing protocol
already supports it.

Everything in Tier 1 is a prerequisite for Tier 2, so none of it gets thrown
away later.

### Tier 2: host-driven campaign

The host's script runs; the events in §2.2 replicate. Any player can *take
part* in a mission, but the host's position and state are what the script
tests, so the host is the one who walks into the marker to start it.

This is a complete co-op campaign, and where most co-op mods for single-player
games stop.

### Tier 3: any player can trigger

The remaining gap is that mission triggers test one player. re3's opcode table
offers about fifteen candidates (`IS_PLAYER_IN_AREA_*`, `_ANGLED_AREA_*`,
`_ON_FOOT_*`, `_IN_CAR_*`, `IS_PLAYER_IN_GANGZONE`, …), which would be an
uncomfortable amount of surface to intercept correctly.

The shipped campaign uses three of them. Measured over all 78 mission
scripts in `reference/scm` (§5):

| Opcode | Call sites |
|---|---|
| `IS_PLAYER_IN_AREA_3D` | 63 |
| `IS_PLAYER_IN_AREA_2D` | 57 |
| `IS_PLAYER_IN_ZONE` | 3 |
| **total** | **123** |

None of the angled, on-foot or in-car variants appear anywhere in the campaign,
and neither does `IS_PLAYER_IN_GANGZONE`. So Tier 3 is: intercept two opcodes
that matter and one that barely does, and return true when any connected
player satisfies the test.

That turns "the host starts missions" into "anyone starts missions" without the
script ever learning that more than one player exists. Intercepting at the
opcode layer also beats faking `CWorld::Players[0]`: the script asks specific
questions, and we only want to change the answers to those.

Intercept point: `CRunningScript::ProcessOneCommand` (`Script.cpp:2050`), or
the individual handlers (see §6).

---

## 5. The decompiled SCM, and why we never ship one

`reference/scm` is a Sanny Builder decompilation of the campaign script
(<https://github.com/Lighnat0r-pers/GTA-III-SCM-Converted>), cloned beside
`reference/re3` under the same rules: offline reference, gitignored, never
committed, never vendored. It carries no licence either, so treat it strictly
as a map.

It maps the *script* the way re3 maps the *engine*. What it is for:

- `triggers.sc` and the per-mission files: the real trigger surface, and how
  §4's table went from fifteen guessed opcodes to three measured ones.
- `GTA 3 Main Variables Named.txt` gives named globals, so mission-progress
  state in `ScriptSpace` can be read while debugging instead of guessed at.
- Knowing which missions do something structurally unusual before a bug
  report blames the netcode.

CoopIII never modifies or redistributes `main.scm`. The host runs the player's
own stock script, byte for byte. Two hard requirements force that:

1. **1:1 fidelity.** A patched campaign script is a different campaign.
2. **Mod compatibility.** Mod Loader mods that replace or extend `main.scm`
   would conflict instantly with a CoopIII-supplied one, and `docs/compat.md`
   commits us to being the well-behaved guest.

The corollary is that the player's `main.scm` is an input we do not control: a
script mod may have replaced it. The host's copy is the one that runs, so
clients do not need a matching file, and the problem only ever exists on the
host.

> Note: `data/main.scm` is ~600 KB, far larger than `SIZE_SCRIPT_SPACE`
> (160 KB). It is a multi-script file (`bUsingAMultiScriptFile`,
> `MultiScriptArray`, `MAX_NUM_MISSION_SCRIPTS = 120`): the main script loads
> into the 128 KB region and mission scripts stream one at a time into the
> 32 KB mission slot. So only one mission ever runs at once, which is why §2
> can speak of "the" mission.

## 6. Open questions

- Does suppressing `CTheScripts::Process()` on clients break anything
  non-mission? The main script also drives pickups, garages, save points and
  the intro. Clients may need those effects replicated too, or may need a
  filtered script run. Not yet investigated, and the first thing Tier 2 work
  should settle.
- `MAX_NUM_SCRIPTS = 128` and one mission at a time
  (`bAlreadyRunningAMissionScript`) are fine for co-op, but worth confirming
  nothing in the campaign relies on per-player script instances.
- Whether Tier 3's opcode interception is better done by detouring
  `ProcessOneCommand` once and switching on the opcode, or by patching the
  individual handlers. The former is one hook; the latter is more surgical.
  Decide when Tier 3 starts, not before.
