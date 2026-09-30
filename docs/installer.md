# CoopIII-Setup

One download turns a player's own v1.0 copy of GTA III into a working CoopIII
install: the Essential Pack, CoopIII.asi, the launcher and CoopIII.ini. A copy
that is not v1.0 is either downgraded by a patch, when one exists for that
exact build and is small, or - the Steam copy - sent to a community downgrade
guide first, with the Setup touching nothing. This is how it does that, where
every file comes from, and what is still missing.

Code: `installer/core` (no window, all tested by `tools/installertest`),
`installer/gui` (the four screens), `installer/assets/components.json` (the
manifest, compiled in), `tools/mkpatch` (builds a downgrade patch),
`tools/release/stage.ps1` (stages a release).

## 1. What the Setup does

1. **Finds the game** (`launcher::FindGameDir`, or Browse) and looks at
   gta3.exe: MD5, size, whether it is running, and which build the manifest's
   `builds` table says it is.
2. **Downgrade screen**, only when gta3.exe is not v1.0. Two cases:
   - **There is a patch for this MD5** (a `*.c3patch` beside the Setup whose
     header names it, or a `patches` entry in the manifest, downloaded from
     CoopIII's release and SHA-256 checked). Continue makes the downgrade the
     install's first step; nothing is patched on this screen.
   - **There is not**, which today is every build but v1.0, the Steam exe
     included. The Setup changes nothing. The screen says "Your GTA III is the
     Steam version. CoopIII needs v1.0. Downgrade your own copy following the
     guide, keep a backup of the original gta3.exe, then press Check again."
     (for an MD5 the manifest does not know, "This gta3.exe is not a build the
     Setup knows" and the MD5). Its buttons: the guide the manifest's
     `downgrader` names, a **More info** link, and **Check again**, which
     re-hashes gta3.exe and moves on to the components once it is v1.0.
3. **Components screen.** Required: CoopIII, Ultimate ASI Loader,
   SilentPatch. The rest are ticked as the manifest says. Hovering a tile
   shows its licence and homepage. Install is enabled once gta3.exe is v1.0 or
   about to be, and GTA III is not running. An **Uninstall** button appears
   when this folder has a `CoopIII-Setup.record`.
4. **Install**, on a worker thread (`InstallJob`):
   - refuses up front if GTA III is running or the folder is not writable
     (the message says to run as administrator);
   - downgrades first; if that fails every other step is skipped, so no mod
     lands on an exe CoopIII cannot use;
   - each download goes to `%LOCALAPPDATA%\CoopIII\Setup\downloads`, as
     `<name>.part` until complete, resumed with an HTTP Range request after a
     drop, retried 4 times with 1/2/4 s back-off, then the next source
     (`url`, then each of `mirrors`). A file is kept only if its SHA-256 is
     the manifest's. A cached file with the right hash is not fetched again,
     so re-running after a failure is quick. https only, redirects may not
     drop to http;
   - zips are read with miniz; an entry whose path could leave the game folder
     is refused; only the files the component's `extract` rules name are
     written, and a rule that matches nothing fails the component (the archive
     is not the one the manifest was written for);
   - every write is atomic (write beside, rename over).
5. **Done screen**: game version (and where the original exe went), the
   launcher button, optional desktop shortcut.

### The rules for files in the game folder

- A file already there with the same bytes is left and not claimed.
- A file already there that is somebody else's is copied to
  `CoopIII-Setup-backup\<same path>` before being replaced.
- An existing `.ini` is never overwritten; it holds the player's settings.
  `defaults` (our `scripts\global.ini`, `FramerateVigilante.ini`) are written
  only where nothing exists.
- Everything written is listed in `CoopIII-Setup.record` (plain text: `file`,
  `dir`, `exe` lines, with the SHA-256 written and the backup path).
- gta3.exe's original goes to `gta3.exe.bak`, or `gta3.exe.<md5>.bak` if a
  different `.bak` is already there. It is written and read back before the
  exe is replaced.

### Uninstall

Reads the record backwards: a file still exactly as written is removed and
its backup (if any) moved back; a file the player changed since is left and
reported; gta3.exe goes back to the original if it is still the v1.0 the
Setup made (if Steam or the player replaced it meanwhile, it is left and the
backup stays); folders the Setup created are removed if empty. Whatever could
not be undone stays in the record, so running it again retries only that.

## 2. The downgrade

### Why a patch and not an exe

CoopIII never ships gta3.exe, in the repo, a release or the Setup. The Setup
turns the player's own exe into v1.0 with a delta that is useless without that
exe, and only when that delta is small (the ceiling below). A build for which
no such delta exists, like the Steam one, is left to the player to downgrade
with a community guide.

### The format (`C3PATCH2`)

`installer/include/installer/core.h` has the byte layout. The differ is
bsdiff's algorithm (hash index over 8-byte windows in place of a suffix array):
for each stretch it stores the byte-wise difference from the aligned old
bytes, plus the bytes that exist only in the new file, and deflates the three
streams. Two compiles of one program differ mostly in moved addresses, which
bsdiff turns into near-zero difference bytes; a copy/insert delta would store
those instructions as literal new bytes, i.e. ship large parts of the game.

Measured on two real compiles of our own `CoopIII.asi` (v16 → v17,
366 592 → 368 128 bytes): patch 21 156 bytes (5.7 %), 10 878 bytes (3.0 %) of
the output stored as themselves. On a synthetic "second build" of the real
v1.0 exe (8 KB inserted, bytes flipped): 2.6 KB patch, 0 bytes of v1.0 stored.

Both ends are verified: the patch names the MD5 and size it applies to and the
MD5 it produces; `ApplyPatch` refuses any other input, refuses output that is
not exactly that MD5, and the Setup additionally refuses a patch whose output
is not `GAME_MD5`. `mkpatch` applies every patch it writes before saying it
worked.

### The Steam exe: no patch, on purpose

The Steam gta3.exe (app 12100, depot 12101, manifest `1807349195210842305`,
build id 2743) is **MD5 `12BDB699AA2F4922240C454C2E567F3F`, 2 801 664 bytes**.
It is wrapped in SteamStub: a `.bind` section and encrypted code. A patch from
it to v1.0 was measured at 984 KB with **86.5 % of v1.0 stored as itself**,
because almost nothing of v1.0's code exists in the encrypted file. Shipping
that would be shipping Rockstar's exe; making it small would mean stripping
SteamStub in a public tool, which is DRM circumvention. Both are out, so:

- The manifest lists the Steam exe in `builds` as "Steam", with **no patch**.
  The Setup names it and stops before touching anything (section 1, step 2).
- The player downgrades their own copy by hand, following the page the
  manifest's `downgrader` entry names, then presses Check again:

  ```json
  "downgrader": {
    "name": "Steam to 1.0 downgrade guide (GTAForums)",
    "url": "https://gtaforums.com/topic/973108-gta-3-steam-to-10-retail-downgrade-and-moddingfixes-tutorial/",
    "infoName": "More info",
    "infoUrl": "https://www.pcgamingwiki.com/wiki/Grand_Theft_Auto_III"
  }
  ```

  These are pages that explain the downgrade, never a file. `Manifest::Parse`
  refuses a `downgrader` link that is not https or that ends in `.exe`, `.zip`,
  `.7z`, `.rar`, `.msi`, `.dll` or `.asi`, and an empty `url` leaves the button
  off rather than pointing anywhere. Changing the recommendation is a manifest
  edit and a rebuild of the Setup, no code.
- **The ceiling.** `mkpatch` refuses to write a patch that stores more than
  25 % of v1.0 as itself (`kMaxStoredFraction`; `--max-stored <percent>`
  moves it), and `tools/release/stage.ps1` refuses to stage one over
  `-MaxStoredPercent` (default 25). Run against the Steam exe, mkpatch prints
  why and writes nothing. Two compiles of one program land in single figures.
- The patch machinery stays for a build where a real patch is small: an
  unwrapped exe such as retail 1.1 or the Rockstar Games Launcher one, if
  someone measures it under the ceiling. Their MD5s are not in the manifest
  yet, because none were available to measure; unknown builds get the generic
  message. For such a build:

  ```
  xmake build mkpatch
  build\windows\x86\release\mkpatch.exe <that gta3.exe> D:\CoopIII\reference\bin\gta3.exe gta3-<build>-v10.c3patch "<build name>"
  ```

  mkpatch prints the size, the share stored as itself, the SHA-256 and the
  `builds`/`patches` lines to paste in; stage it with `stage.ps1 -Patch`.

## 3. Components

Every file below was downloaded and hashed on 2026-09-26. "Tested" means the
extracted files are byte-identical to the ones in the owner's own working mod
pack (`essentials-mod-pack_1741967254_825844`).

| Component | Version | Source (primary) | Licence | Ours to rehost? | Where it lands | Tested |
|---|---|---|---|---|---|---|
| CoopIII | 0.1.2 | inside the Setup | CoopIII's | yes | `CoopIII.asi`, `CoopIII.ini`, `coopiii-launcher.exe` | - |
| Ultimate ASI Loader (ThirteenAG) | v7.8.0 | github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/v7.8.0/Ultimate-ASI-Loader.zip | MIT | mirror | `dinput8.dll`; `scripts\global.ini` if absent | newer than the pack's (its exact build is not a published release) |
| SilentPatch III (Silent) | 1.1.9.1 (release 1.1-BUILD33.1-SA) | github.com/CookiePLMonster/SilentPatch/releases/download/1.1-BUILD33.1-SA/SilentPatchIII.zip | MIT | mirror | `SilentPatchIII.asi`, `.ini` | identical |
| SilentPatch DDraw (Silent) | 1.1.6.0 (same release) | .../1.1-BUILD33.1-SA/SilentPatchDDraw.zip | MIT | mirror | `ddraw.dll` | identical |
| Widescreen Fix (ThirteenAG) | rolling `gta3` tag, asset of 2026-09-25 | github.com/ThirteenAG/WidescreenFixesPack/releases/download/gta3/GTA3.WidescreenFix.zip | MIT | mirror | `scripts\GTA3.WidescreenFix.asi`, `.ini` (the zip's own `d3d8.dll` and `global.ini` are not installed) | **newer** than the pack's |
| Framerate Vigilante (Junior_Djjr) | III build from the pack | CoopIII release only: `FramerateVigilante.III.asi` | MIT (github.com/GTAmodding/FramerateVigilante) | bundled, with its licence | `FramerateVigilante.III.asi`; `.ini` (60 FPS) if absent | identical |
| GInput (Silent) | 1.11 | silent.rockstarvision.com/uploads/GInputIII.zip (linked from silentsblog.com) | none published | **no** | `GInputIII.asi`, `.ini`, `models\*.txd` | identical |
| CLEO (cleolibrary) | 2.0.0.6 | github.com/cleolibrary/III.VC.CLEO/releases/download/2.0.0.6/III.CLEOv2.0.0.6.zip | binaries say MIT; the repo has no licence file | **no** | `III.CLEO.asi`, `CLEO\` | identical |
| Mod Loader (LINK/2012) | 0.3.7 | github.com/thelink2012/modloader/releases/download/v0.3.7/modloader.zip | MIT | mirror | `modloader.asi`, `modloader\` | identical |
| Windowed Mode (ThirteenAG), off by default | v2.2 | github.com/ThirteenAG/III.VC.SA.WindowedMode/releases/download/v2.2/III.VC.SA.WindowedMode.zip | MIT | mirror | `III.VC.SA.WindowedMode.asi` | not in the pack |
| SACarCam (erorcun), off by default | r6 | github.com/erorcun/SACarCam/releases/download/r6/SACarCam.dll | **none** | **never** | `SACarCam.asi` (saved under that name so the loader picks it up); see compat.md §2.7 | not in the pack |

"Mirror" = an unmodified copy of the same upstream file is attached to
CoopIII's release as `mirror-*.zip` and listed in `mirrors`; it is used only
when the upstream download fails or has changed. That matters most for the
Widescreen Fix, whose `gta3` tag is re-uploaded in place: the day ThirteenAG
publishes a new build, the upstream hash stops matching and the Setup falls
back to the mirror. Bumping it is a manifest edit (url stays, new sha256, new
mirror name).

Left out of the pack on purpose: **CrashInfo** (MIT, but only published on
MixMods, needs `libcurl.dll`/`zlib1.dll` and fetches updates itself),
**noDEP** and **RunDLL32 Fix** (no licence and no stable download),
`dsound_x64.dll` (a 64-bit DLL, never loaded by a 32-bit game) and
`models\particle.txd` (a modified game file).

## 4. The manifest

`installer/assets/components.json`. Top level: `builds` (md5, size, name),
`patches` (from, name, file, url, mirrors, sha256), `downgrader` (name, url,
infoName, infoUrl: section 2), `components`. A component:

| Key | Meaning |
|---|---|
| `id`, `name`, `description`, `version`, `license`, `homepage` | shown on the tile and its tooltip |
| `required` | always installed, cannot be unticked |
| `selected` | ticked when the Setup opens (default true) |
| `source` | `local` (inside the Setup) or `download` |
| `files`, `destination` | local: payload names and the folder they go to |
| `url`, `mirrors`, `sha256`, `size` | download: sources in order, and the hash that decides |
| `archive` | `zip` or `file` |
| `extract` | zip: `{from, to}`; a `from` ending in `/` takes the whole folder |
| `saveAs` | file: the name to save it under (SACarCam.dll → SACarCam.asi) |
| `defaults` | `{file, to}`: a payload file written only if `to` does not exist |

`installertest` holds the manifest to its rules: every component installable,
every source https, every mirror a CoopIII release asset, every zip with
extract rules, a licence named on each, and SACarCam, GInput and CLEO never
mirrored.

## 5. Releasing

`tools/release/stage.ps1` builds (unless `-SkipBuild`) and fills
`build/release-staging` with exactly what a release carries: the Setup,
CoopIII.asi, coopiii-launcher.exe, server.exe, the `mirror-*.zip` files
(fetched from upstream and hash-checked), FramerateVigilante.III.asi and its
licence, any `.c3patch` the manifest names (only under the stored-bytes
ceiling), THIRD-PARTY-NOTICES.txt and
SHA256SUMS.txt. It refuses to stage any file whose MD5 is a known gta3.exe
build, and refuses to rehost a component that is not MIT.

The website's Download button points at
`https://github.com/noxxUY/CoopIII/releases/latest/download/CoopIII-Setup.exe`,
and the manifest's mirrors and patches at `releases/latest/download/<name>`, so
every release must carry all of them (the stage script sees to that).
`.github/workflows/release.yml` runs the same script on a `v*` tag and
attaches the folder; on CI, Framerate Vigilante is fetched from the previous
release, so the **first** release has to be staged and uploaded by hand.

## 6. Not proven yet

- The Steam path ends at the guide: the Setup cannot check that a player
  downgraded correctly beyond the MD5 Check again reads, which is the check
  that matters. The guide and the wiki page are third-party pages CoopIII
  does not control.
- The GUI has been built, not clicked through. Every step it drives is
  covered by `installertest` (offline, plus `--online`, which installed the
  real pack from the internet into a scratch folder and uninstalled it
  cleanly), but the screens themselves have not been looked at since the
  changes.
- The installed stack has not been run in GTA III: Ultimate ASI Loader v7.8.0
  and today's Widescreen Fix are newer than what the owner plays with.
- Program Files installs need the Setup to be run as administrator; the Setup
  says so rather than elevating itself.
