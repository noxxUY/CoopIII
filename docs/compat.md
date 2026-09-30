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

CoopIII-Setup installs this stack from each project's own release (the same
files, byte for byte, for SilentPatch, CLEO, Mod Loader, GInput and Framerate
Vigilante), in the game root rather than under `modloader/_ESSENTIALS`; the
Widescreen Fix goes to `scripts/`. `docs/installer.md` has the table.

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

### 2.7 A San Andreas car camera (SACarCam)

`github.com/erorcun/SACarCam` puts San Andreas' car camera into III (and VC).
It is an optional mod the player installs on their own. CoopIII does not ship
it, bundle it, patch it or contain any of its code, and must not: the repo
has **no licence file and no licence text anywhere in it**, so all rights are
reserved by its author. Reading it to check compatibility is fine; copying it,
redistributing a build of it, or writing "our own" version with its source
open beside us is not. CoopIII only makes sure the two run together.

**It runs on our exe unmodified.** Its `DllMain` identifies III 1.0 by
`*(DWORD*)0x005C1E70 == 0x53E58955` (and `0x005C1E75 == 0xB85548EC`); both
hold on the pinned 1.0 image. Every address it uses is hardcoded with no
pattern scanning, and it has no III 1.1 or Steam addresses at all: on those
builds it patches nothing. The r6 release binary
(`SACarCam.dll`, 56 832 bytes, SHA-256 `65D06692…FD320`) was disassembled and
patches exactly what its source says. On III it writes, in `DllMain`:

| Site | Retail | What it becomes |
|---|---|---|
| `0x00456F40` | `WellBufferMe` entry (`sub esp,8`) | `jmp` to its own copy, which also records the previous camera mode |
| `0x00459A54` | `call CCam::Process_Cam_On_A_String` (MODE 18) | its SA car camera |
| `0x00459B36` | `call CCam::Process_BehindBoat` (MODE 22) | the same camera for boats |
| `0x005225D2` | FireTruckControl `call CPad::GetCarGunLeftRight` | 0 while the car camera is up |
| `0x0052260E` | FireTruckControl `call CPad::GetCarGunUpDown` | 0 while the car camera is up |
| `0x0053D628` | TankControl `call CPad::GetCarGunLeftRight` | 0 while the car camera is up |
| `0x0048BFB0` | CGame::Initialise `call 0x004735A0` | its debug menu registration, then the original |

Its camera then aims the tank's turret and the fire truck's cannon at where
the camera looks (writing `CAutomobile` +0x580/+0x584 and playing the
turret's motor, sound 26), in place of the stick.

**No overlap with anything CoopIII hooks.** None of those seven sites is one
of CoopIII's call-site redirects (the camera call `0x0048C9B5`, the two stunt
shot calls, the passenger aim's four collision calls at `0x004605E5`,
`0x00460691`, `0x00460923`, `0x00460B8E`, the gun gates' `FindPlayerVehicle`
calls at `0x0052259A` and `0x0053D5E5`, or any other), and none is inside
the first bytes of a function CoopIII detours; every hex constant in
`client/src` was checked against them. `tools/clienttest` (carcam.cpp) pins the
camera ones. Load order does not matter: SACarCam patches in `DllMain`,
CoopIII on the first frame, and CoopIII's redirects all verify their target
first.

What works with it, by construction rather than by test:

- **Drivers.** The SA camera follows the car; the turret and cannon it turns
  are sampled by `SyncCarExtras` like any other, and the water cannon still
  goes through `CWaterCannons::UpdateOne`.
- **Passengers.** A rider's camera follows the car he rides in (FindPlayerVehicle
  does not care which seat), so he gets the SA camera and can look round with
  the mouse. `ridecam` still corrects that car before the camera looks.
- **The passenger's free aim.** Holding the right button still hands the
  rider `MODE_FOLLOWPED` through `TakeControl`; SACarCam never touches that
  mode or its collision calls. Letting go restores the car camera, which
  starts fresh behind the car.
- **Cutscenes and the unique jump's shot** use other modes (FLYBY, FIXED).

**The one thing CoopIII changes** (`client/src/game/carcam.*`): SACarCam turns
the gun of whatever car the camera follows, and for a rider that is somebody
else's tank. `CorrectCarExtras` wrote the driver's aim back after the frame,
so it was drawn right, but the turret's motor played on the rider's machine
every frame his camera and the driver's gun disagreed. When the car camera's
call leads into another module, CoopIII now takes that call and chains to it,
and for a tank or fire truck the local player is not driving it puts the gun
back afterwards and sets the car's `m_audioEntityId` to -1 for the length of
the call (cAudioManager::PlayOneShot ignores a negative entity). Against the
retail camera nothing is taken. The gun gates (`client/src/game/cargun.*`)
sit in front of SACarCam's three stick calls, so for a passenger those are
never reached and only the camera's own write is left to put back. The log
says which case it found:
`carcam: the car camera call at 0x00459A54 leads into SACarCam.asi ...`.

**Installing it** (the player's side): download `SACarCam.dll` from the
project's r6 release, rename it `SACarCam.asi` and put it next to
`CoopIII.asi`. Nothing in `CoopIII.ini` turns it on or off; remove the file to
go back. GInput is optional for it (it falls back to a dummy pad). Its
`LCSCarCam` build patches the same sites.

**Not proved in a running game:** that the SA camera behaves for a rider in a
car whose transform arrives over the wire (its history-based trailing might
swing on a snap), that the mute leaves no other sound of the car silenced,
and the whole thing under two clients.

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
