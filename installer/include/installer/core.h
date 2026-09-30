// The Setup's working parts, with no interface attached: what it can install,
// how it proves a download is what it claims to be, how it puts a Steam copy
// of GTA III back to v1.0, and how it takes all of that out again.
//
// The window is installer/gui. Everything here is testable without one, which
// matters most for the things that touch a player's files: the patch, the
// hash, the archive reader and the install record.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace coopiii::installer {

// ---- SHA-256 --------------------------------------------------------------
//
// FIPS 180-4. Here rather than as a dependency because it is sixty lines and
// the only thing the Setup hashes is a handful of downloads.

std::string Sha256(const void *data, size_t length);   // lowercase hex
std::string Sha256File(const std::string &path);       // empty if unreadable

// ---- the manifest ---------------------------------------------------------
//
// installer/assets/components.json, compiled into the Setup. docs/installer.md
// describes every key.

// One file or folder taken out of a downloaded archive.
//
//   from  a path inside the archive. Ending in '/' means everything under
//         that folder, keeping the rest of each path.
//   to    where it lands, relative to the game folder. Same rule for '/'.
struct ExtractRule {
	std::string from;
	std::string to;
};

// A file the Setup carries that is written only when the player has none.
// The ASI loader's global.ini is one: the settings CoopIII was tested with,
// and nobody's own settings overwritten.
struct DefaultFile {
	std::string file;   // name in the Setup's payload
	std::string to;     // relative to the game folder
};

struct Component {
	std::string id;
	std::string name;
	std::string description;
	std::string version;
	std::string license;
	std::string homepage;
	bool        required = false;
	bool        selected = true;    // ticked when the Setup opens

	// Where the files come from.
	//
	//   local     carried inside the Setup; that is CoopIII itself
	//   download  fetched from `url` (then each of `mirrors` if that fails)
	//             and checked against `sha256` before anything is unpacked
	std::string source;
	std::string url;
	std::vector<std::string> mirrors;
	std::string sha256;
	uint64_t    size = 0;           // informational; the hash is the check

	// "zip" or "file". A plain file lands under `destination` as `saveAs`, or
	// as the leaf of its url when that is empty - SACarCam is published as a
	// .dll and has to be an .asi for the loader to pick it up.
	std::string archive = "file";
	std::string saveAs;
	std::vector<ExtractRule> extract;

	// For a local component, the payload files to copy, into `destination`.
	std::vector<std::string> files;
	std::string destination;

	std::vector<DefaultFile> defaults;

	bool Downloadable() const { return source == "download"; }
	bool Ready() const {
		return source == "local" ||
		       ((!url.empty() || !mirrors.empty()) && sha256.size() == 64);
	}
	std::vector<std::string> Sources() const;   // url then mirrors, empties dropped
};

// A gta3.exe the Setup can put a name to.
struct Build {
	std::string md5;       // uppercase hex
	uint64_t    size = 0;
	std::string name;      // "v1.0 retail", "Steam"
};

// A downgrade patch the Setup knows how to fetch.
struct PatchSource {
	std::string from;      // MD5 of the gta3.exe it applies to, uppercase
	std::string name;      // which build that is, for the window
	std::string file;      // the name it is saved under
	std::string url;
	std::vector<std::string> mirrors;
	std::string sha256;    // of the patch file
};

// Where a player is sent when their gta3.exe has no patch: a guide to
// downgrading it themselves, which they follow before running the Setup
// again. A Steam exe is wrapped in SteamStub, so a patch from it would have to
// carry most of v1.0 as it is (or strip the wrapper), and CoopIII does
// neither. Kept in the manifest so the recommendation can change without new
// code. Only pages, never a file: Parse refuses a url that looks like a
// download, and an empty url means the window offers no link at all.
struct Downgrader {
	std::string name;       // the button's label
	std::string url;
	std::string infoName;   // a second, background link
	std::string infoUrl;
};

struct Manifest {
	std::vector<Build>       builds;
	std::vector<PatchSource> patches;
	std::vector<Component>   components;
	Downgrader               downgrader;

	// Reads the JSON the Setup carries. Returns false and fills `error` if the
	// file is not the shape this expects - a manifest that half-parsed would
	// install a half Essential Pack.
	bool Parse(const std::string &json, std::string *error);

	const Component   *Find(const std::string &id) const;
	const Build       *FindBuild(const std::string &md5) const;
	const PatchSource *FindPatch(const std::string &fromMd5) const;
};

// The manifest built into the Setup. `error` says why, if it came back empty.
const Manifest &BuiltInManifest(std::string *error = nullptr);

// ---- the downgrader -------------------------------------------------------
//
// A patch turns one exact gta3.exe into the v1.0 retail one. The repo carries
// no gta3.exe and never will; it carries the patch, built by tools/mkpatch
// from two copies the project owner has, and published as a release asset.
//
// The differ is bsdiff's: it finds where the old file lines up with the new
// one, and for each stretch stores the byte-wise difference from the old
// bytes (mostly zeros, because two builds of one program mostly differ in the
// addresses inside otherwise identical instructions) plus whatever is
// genuinely new. Each of the three streams is deflated on its own.
//
// Format, all little-endian:
//
//   "C3PATCH2"             8 bytes
//   uint32 oldSize         what it expects to be handed
//   char   oldMd5[32]      uppercase hex, the exe this patch was built from
//   uint32 newSize         what it will produce
//   char   newMd5[32]      uppercase hex, v1.0 retail
//   uint32 controls        how many control entries
//   then three sections, control, diff and extra, each:
//     uint32 rawSize
//     uint32 packedSize
//   then the three packed payloads in that order (zlib streams).
//
//   A control entry is uint32 diffLen, uint32 extraLen, int32 seek:
//   add diffLen diff bytes to the old file at the cursor, then copy extraLen
//   bytes from extra, then move the old cursor by seek.

struct PatchInfo {
	uint32_t    oldSize = 0;
	uint32_t    newSize = 0;
	std::string oldMd5;
	std::string newMd5;
	uint32_t    controls   = 0;
	uint32_t    extraBytes = 0;   // bytes of the output stored as themselves
};

// Reads just the header. Lets the Setup say "this patch is for a different
// build" before touching anybody's game.
bool ReadPatchInfo(const std::string &patchPath, PatchInfo *info, std::string *error);
bool ReadPatchInfoBuffer(const std::vector<uint8_t> &patch, PatchInfo *info, std::string *error);

// How much of the output a patch carries as itself, 0 to 1.
double StoredFraction(const PatchInfo &info);

// The most of v1.0 a patch may carry as itself before it stops being a patch
// and becomes a copy of the game. mkpatch and tools/release/stage.ps1 both
// refuse anything over it. Two compiles of one program land in single
// figures; the SteamStub-wrapped Steam exe measured 86.5%.
constexpr double kMaxStoredFraction = 0.25;

// Builds a patch. Used by tools/mkpatch.
bool MakePatch(const std::string &oldPath, const std::string &newPath,
               const std::string &patchPath, std::string *error);

// Applies it. Verifies the MD5 of what it was handed before starting and the
// MD5 of what it produced before replacing anything, so a patch can only ever
// turn the exact build it was made for into the exact build it was made to
// produce. The exe is replaced by a rename, never rewritten in place.
//
// `backupPath` is written first when it is not empty, so the original survives.
bool ApplyPatch(const std::string &exePath, const std::string &patchPath,
                const std::string &backupPath, std::string *error);

// The same two on buffers, which is what the tests use.
bool MakePatchBuffers(const std::vector<uint8_t> &oldBytes, const std::vector<uint8_t> &newBytes,
                      std::vector<uint8_t> *patch, std::string *error);
bool ApplyPatchBuffers(const std::vector<uint8_t> &oldBytes, const std::vector<uint8_t> &patch,
                       std::vector<uint8_t> *newBytes, std::string *error);

// ---- archives -------------------------------------------------------------

struct ArchiveEntry {
	std::string          path;   // forward slashes, as stored
	std::vector<uint8_t> bytes;
};

// Every file in a zip, or false with a reason. Refuses an entry whose path
// could climb out of the folder it is unpacked into.
bool ReadZip(const std::vector<uint8_t> &zip, std::vector<ArchiveEntry> *out, std::string *error);

// Applies a component's extract rules to what came out of its archive: the
// game-folder path for each entry that is wanted. An entry no rule names is
// left out. Fails if a rule matched nothing, since that means the archive is
// not the one the manifest was written against.
bool PlanExtract(const std::vector<ArchiveEntry> &entries, const std::vector<ExtractRule> &rules,
                 std::vector<std::pair<std::string, const ArchiveEntry *>> *plan,
                 std::string *error);

// Builds a zip, stored, not deflated. Only the tests and the release script
// need one.
std::vector<uint8_t> WriteZip(const std::vector<ArchiveEntry> &entries);

// ---- downloading ----------------------------------------------------------

// Fetches `url` into `dest`, over https only. Continues a `dest`.part left by
// an earlier attempt when the server allows it, and retries a failed attempt
// a few times before giving up. `progress` gets (bytes so far, total or 0).
using ByteProgress = std::function<void(uint64_t, uint64_t)>;
bool DownloadFile(const std::string &url, const std::string &dest, const ByteProgress &progress,
                  const std::function<bool()> &cancelled, std::string *error);

// Tries each source in turn and keeps the first whose SHA-256 is `sha256`.
// A file already in `dest` with the right hash is not fetched again, which is
// what makes a second run after a failed one quick.
bool FetchVerified(const std::vector<std::string> &sources, const std::string &sha256,
                   const std::string &dest, const ByteProgress &progress,
                   const std::function<bool()> &cancelled,
                   const std::function<void(const std::string &)> &log, std::string *error);

// Where downloads are kept between runs: %LOCALAPPDATA%\CoopIII\Setup\downloads.
std::string DefaultCacheDir();

// ---- the install record ---------------------------------------------------
//
// Every file the Setup writes into a game folder is listed in
// CoopIII-Setup.record there, with the hash it wrote and where the file it
// replaced was put. Uninstall reads it back: what is still exactly what the
// Setup wrote is removed, what was replaced comes back, and gta3.exe goes back
// to the build it was before the downgrade.

constexpr const char *kRecordName = "CoopIII-Setup.record";
constexpr const char *kBackupDir  = "CoopIII-Setup-backup";

struct RecordEntry {
	enum class Kind : uint8_t { File, Dir, Exe };
	Kind        kind = Kind::File;
	std::string path;      // relative to the game folder
	std::string hash;      // File: SHA-256 written. Exe: MD5 before the downgrade.
	std::string backup;    // relative; empty if nothing was there before
};

struct InstallRecord {
	std::vector<RecordEntry> entries;

	bool Load(const std::string &gameDir, std::string *error);
	bool Save(const std::string &gameDir, std::string *error) const;
	RecordEntry *Find(RecordEntry::Kind kind, const std::string &path);
};

bool HasInstallRecord(const std::string &gameDir);

// Takes out what the record says the Setup put in. `log` gets one line per
// thing it did or refused to do. Returns false if anything could not be put
// back; the record then keeps just those entries so a second run can retry.
bool Uninstall(const std::string &gameDir, std::vector<std::string> *log, std::string *error);

// ---- installing -----------------------------------------------------------

enum class StepState : uint8_t { Waiting, Working, Done, Failed, Skipped };

struct StepProgress {
	std::string id;
	std::string name;
	StepState   state = StepState::Waiting;
	std::string note;      // "Done", "Waiting", "2.1 of 7.7 MB", or why it failed
};

// What the window watches while the work happens on another thread.
struct Progress {
	std::vector<StepProgress> steps;
	std::vector<std::string>  details;   // the "[ ok ]" / "[ .. ]" log
	int    finished  = 0;
	int    total     = 0;
	bool   running   = false;
	bool   cancelled = false;
	bool   failed    = false;
	bool   downgraded = false;
	std::string backup;                  // where the original gta3.exe went
};

// The id of the step that downgrades gta3.exe, when it is asked for.
constexpr const char *kDowngradeId = "gta3-downgrade";

struct InstallOptions {
	std::string              gameDir;
	std::vector<std::string> components;   // ids, installed in the manifest's order
	bool                     downgrade = false;
	std::string              cacheDir;     // empty: DefaultCacheDir()
	const Manifest          *manifest = nullptr;   // null: BuiltInManifest()
};

// Runs the install on a thread of its own. Everything the window reads goes
// through Snapshot(), which takes the lock - the work is file IO and network,
// and neither belongs on the frame loop.
class InstallJob {
public:
	InstallJob();
	~InstallJob();

	InstallJob(const InstallJob &)            = delete;
	InstallJob &operator=(const InstallJob &) = delete;

	void Start(const InstallOptions &options);
	void Start(const std::string &gameDir, const std::vector<std::string> &chosen) {
		InstallOptions o;
		o.gameDir    = gameDir;
		o.components = chosen;
		Start(o);
	}
	void Cancel();

	bool     Running() const;
	Progress Snapshot() const;

private:
	struct Impl;
	Impl *m_impl;
};

// ---- the game folder ------------------------------------------------------

// What the Setup can tell about a gta3.exe before doing anything to it.
struct GameExe {
	bool        present = false;
	std::string md5;
	uint64_t    size    = 0;
	bool        isV10   = false;
	std::string buildName;     // from the manifest, or empty if unknown
	bool        running = false;

	// How it can be downgraded: a patch beside the Setup, or one the manifest
	// knows where to fetch. Both empty means it cannot be.
	std::string localPatch;
	const PatchSource *knownPatch = nullptr;

	bool CanDowngrade() const { return !isV10 && (!localPatch.empty() || knownPatch); }
};

GameExe InspectGame(const std::string &gameDir, const Manifest &manifest);

// True while something holds gta3.exe open for execution.
bool GameIsRunning(const std::string &gameDir);

// ---- odds and ends the window needs ---------------------------------------

// Puts a shortcut to `target` on the desktop. Returns false if the shell
// refused, which is not worth stopping the install over.
bool CreateDesktopShortcut(const std::string &target, const std::string &name,
                           const std::string &workingDir);

// Where the Setup's own files are.
std::string SetupDir();

// How a "local" component's files are found.
//
// The Setup carries them inside itself, so a release is one exe; it registers
// a reader for them at startup. Without one - in a test, or a build that has
// no payload - the files are looked for beside the executable instead, which
// is what a developer running the Setup out of the build folder has.
using PayloadReader = std::function<bool(const std::string &name, std::vector<uint8_t> *out)>;
void SetPayloadReader(PayloadReader reader);

// ---- file helpers shared by the pieces above ------------------------------

bool ReadWholeFile(const std::string &path, std::vector<uint8_t> *out, std::string *error);

// Writes beside `path` and renames over it, so a failure half way leaves the
// old file whole.
bool WriteFileAtomic(const std::string &path, const std::vector<uint8_t> &bytes,
                     std::string *error);

bool EnsureDir(const std::string &path);

} // namespace coopiii::installer
