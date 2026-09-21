# Coexisting with an existing GTA III mod stack

CoopIII is not the only thing patching this game. The target install already
runs a loader, a compatibility patch, a script engine and a mod manager.
CoopIII has to fit around all of it, not the other way round.

Surveyed install (verified 2026-09-21, from each file's PE version resource):

```
C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto 3
```

## 1. What is actually installed

| Component | File | Version resource |
|---|---|---|
| Ultimate ASI Loader | `dinput8.dll` | (no resource; 1.1 MB, bundles `d3dx9_43`/`ddrawex`/`D3dHook`) |
| SilentPatch DDraw component | `ddraw.dll` | `SilentPatchDDraw` |
| SilentPatch III | `modloader/_ESSENTIALS/SilentPatch/SilentPatchIII.asi` | `SilentPatchIII` |
| Mod Loader | `modloader.asi` | (no resource) |
| CLEO | `III.CLEO.asi` + 5 `CLEO/CLEO_PLUGINS/*.cleo` | `III.CLEO` |
| Widescreen Fix | `modloader/_ESSENTIALS/Widescreen Fix by ThirteenAG/GTA3.WidescreenFix.asi` | `GTA3.WidescreenFix` |
| Windowed Mode | `modloader/_ESSENTIALS/III.VC.SA.WindowedMode.asi` | `III.VC.SA.WindowedMode` |
| Framerate Vigilante | `modloader/_ESSENTIALS/FramerateVigilante/FramerateVigilante.III.asi` | (MixMods) |
| GInput | `modloader/_ESSENTIALS/GInput/` | controller support |
| CrashInfo | `CrashInfo.III.asi` | crash logger |
| noDEP | `_noDEP.asi` | disables DEP |

Roughly, the load chain is:

```
gta3.exe
├── ddraw.dll      SilentPatch's DDraw component
└── dinput8.dll    Ultimate ASI Loader
    ├── CrashInfo.III.asi
    ├── III.CLEO.asi            → CLEO/CLEO_PLUGINS/*.cleo
    ├── _noDEP.asi
    └── modloader.asi
        └── modloader/_ESSENTIALS/…  SilentPatch, Widescreen Fix,
                                     WindowedMode, FramerateVigilante, GInput
```

## 2. Consequences for CoopIII

### 2.1 Ship a `.asi`, not a proxy DLL

The original design had a `proxy/` target that would impersonate one of
`gta3.exe`'s DLL dependencies to get loaded. Both of the slots that trick
normally uses are already taken: `ddraw.dll` is SilentPatch's, `dinput8.dll`
is Ultimate ASI Loader's. Installing a CoopIII proxy would mean evicting a mod
the player deliberately installed.

There is also no need for it. Ultimate ASI Loader is already present and its
entire job is loading `.asi` plugins. CoopIII ships as `CoopIII.asi` and gets
loaded for free. The same file works on a vanilla install, since Ultimate ASI
Loader is the standard way to bootstrap GTA III mods anyway.

### 2.2 Never scan for patterns in `DllMain`

SilentPatch, the Widescreen Fix and Framerate Vigilante all rewrite game code
at runtime. At `DllMain` time:

- the loader lock is held, so there is very little we may safely do;
- `modloader.asi` has not run yet, so SilentPatch has not patched yet.

Scanning then would match the *pre-patch* bytes and hook code that is about to
be overwritten underneath us: an intermittent failure that looks like a CoopIII
bug and is miserable to trace.

CoopIII therefore scans lazily, on the first game frame, once every other
plugin has loaded and finished patching. `DllMain` only records that it was
loaded.

CLEO makes the rule mandatory. The install includes `III.MemoryModule.cleo`, a
plugin whose purpose is letting *scripts* write process memory, so what is
patched is per-user and not knowable ahead of time.

### 2.3 Which is why v1 uses addresses, not patterns

The original plan was to scan for byte patterns everywhere, on the reasoning
that hardcoded offsets break when things shift. Runtime patching inverts that.

Patching changes bytes, not addresses. A function SilentPatch has rewritten is
still at the same entry point; what breaks is a pattern matching the bytes it
rewrote. `tools/sigmaker` verifies uniqueness against the *on-disk* image, so a
pattern that is unique there but patched in memory resolves to `NOT_FOUND` at
load time on this install, for the mods the player wants.

The argument for patterns (surviving a different build) does not apply here,
because `client/src/game/verify.h` refuses to load against anything but the one
build the addresses came from. There is no other build to survive.

So v1 uses absolute addresses, gated on image identity. The scanner stays in
the tree (`client/src/hook/pattern.h`) for the day CoopIII supports a second
exe, and for anchoring anything that turns out to move. The version anchors the
guard itself reads are `.rdata` strings rather than code, for the same reason.

### 2.4 The frame rate is 60, and the engine clock is not a clock

`FramerateVigilante.ini` on this install sets `FPSlimit = 60`.

That does not change the snapshot rate, but only because §1.2 of `protocol.md`
already drives both the send cadence and `sendTimeMs` off CoopIII's own
monotonic wall clock rather than off frames or off `CTimer`.
Anything that counts frames would have broken here: 60 / 25 is not an integer,
and `ms_fTimeStep` varies per frame by construction.

### 2.5 Fail loudly, never silently

With this many plugins patching the same binary, "CoopIII loaded but quietly
did nothing" is the failure mode to design against. Every hook records a
`HookFailure` with a reason (`client/src/hook/hook.h`), and the client reports
them rather than limping on. A mod conflict should produce a message naming the
hook that failed, not a multiplayer session where nobody can see each other.

### 2.6 Entity budget is shared with traffic

`gta3.ini` is *not* a version file. It holds the ped and car density
multipliers (`re3 src/core/IniFile.cpp:13-28`). This install has `1.0` / `1.0`,
giving the stock budget:

```cpp
CPopulation::MaxNumberOfPedsInUse = 25.0f * PedNumberMultiplier;  // 25
CCarCtrl::MaxNumberOfCarsInUse    = 12.0f * CarNumberMultiplier;  // 12
```

8 players plus their vehicles consume a meaningful share of that. Remote
players must not be counted as ambient population, or traffic and pedestrians
will thin out as the session fills up. (v1 leaves NPCs/traffic unsynced
entirely - see `protocol.md` §3 - so this is a v2 concern, recorded here
because the numbers come from the same survey.)

## 3. Not yet verified

- Whether Ultimate ASI Loader on this install loads from the game root, from
  `scripts/`, or both. `scripts/` currently holds only `global.ini`, and every
  installed `.asi` sits in the root or under `modloader/`, which suggests root.
  Worth confirming before we document an install path for users.
- Load order *between* root `.asi` files. It does not matter for CoopIII as
  long as §2.2 holds.
- Whether Mod Loader should be the recommended install route (dropping CoopIII
  into `modloader/`) instead of the game root. Mod Loader's ordering guarantees
  are stronger, but it adds a hard dependency on Mod Loader being installed.
