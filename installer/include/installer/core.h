// The Setup's working parts, with no interface attached: what it can install,
// how it proves a download is what it claims to be, and how it puts a Steam
// copy of GTA III back to v1.0.
//
// The window is installer/gui. Everything here is testable without one, which
// matters most for the two things that touch a player's files: the patch and
// the hash.
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
std::string Sha256File(const std::string &path);

// ---- the components -------------------------------------------------------

struct Component {
	std::string id;
	std::string name;
	std::string description;
	std::string version;
	bool        required = false;

	// Where the files come from.
	//
	//   local     shipped beside the Setup; that is CoopIII itself
	//   download  fetched from `url` and checked against `sha256`
	//
	// Every mod in the Essential Pack is a download, on purpose: CoopIII does
	// not redistribute anybody else's work. The URLs and hashes are left for
	// the project owner to fill in once each licence has been read, which is
	// why a component with an empty url is shown but cannot be installed.
	std::string source;     // "local" or "download"
	std::string url;
	std::string sha256;
	std::string homepage;

	// Where it lands, relative to the game folder. Empty means the root.
	std::string destination;

	// For a local component, the files to copy from beside the Setup.
	std::vector<std::string> files;

	bool Downloadable() const { return source == "download"; }
	bool Ready() const { return source == "local" || (!url.empty() && !sha256.empty()); }
};

struct Manifest {
	std::vector<Component> components;

	// Reads the JSON the Setup carries. Returns false and fills `error` if the
	// file is not the shape this expects - a manifest that half-parsed would
	// install a half Essential Pack.
	bool Parse(const std::string &json, std::string *error);

	const Component *Find(const std::string &id) const;
};

// The manifest built into the Setup.
const Manifest &BuiltInManifest();

// ---- the downgrader -------------------------------------------------------
//
// A patch is a list of differences between the player's gta3.exe and the v1.0
// retail one. The repo carries no gta3.exe and never will; it carries the
// patch, and only once its owner has one to carry.
//
// Format, all little-endian:
//
//   "C3PATCH1"          8 bytes
//   uint32 oldSize      what it expects to be handed
//   char   oldMd5[32]   uppercase hex, the exe this patch was built from
//   uint32 newSize      what it will produce
//   char   newMd5[32]   uppercase hex, v1.0 retail
//   uint32 opCount
//   then opCount of:
//     uint8  kind       0 copy from old, 1 insert literal bytes
//     uint32 a          copy: offset in old.   insert: length
//     uint32 b          copy: length.          insert: unused
//     (insert is followed by `a` bytes)
//
// Uncompressed on purpose. Two builds of the same program share long runs, so
// the copies carry most of the file and only the real differences are stored -
// and an uncompressed format is one that can be read with a hex editor when
// something goes wrong.

struct PatchInfo {
	uint32_t    oldSize = 0;
	uint32_t    newSize = 0;
	std::string oldMd5;
	std::string newMd5;
	uint32_t    ops = 0;
};

// Reads just the header. Lets the Setup say "this patch is for a different
// build" before touching anybody's game.
bool ReadPatchInfo(const std::string &patchPath, PatchInfo *info, std::string *error);

// Builds a patch. Used by tools/mkpatch, which is how the project owner turns
// their own two executables into the file the Setup ships.
bool MakePatch(const std::string &oldPath, const std::string &newPath,
               const std::string &patchPath, std::string *error);

// Applies it. Verifies the MD5 of what it was handed before starting and the
// MD5 of what it produced before replacing anything, so a patch can only ever
// turn the exact build it was made for into the exact build it was made to
// produce.
//
// `backupPath` is written first when it is not empty, so the original survives.
bool ApplyPatch(const std::string &exePath, const std::string &patchPath,
                const std::string &backupPath, std::string *error);

// The same two functions on buffers, which is what the tests use.
bool MakePatchBuffers(const std::vector<uint8_t> &oldBytes, const std::vector<uint8_t> &newBytes,
                      std::vector<uint8_t> *patch, std::string *error);
bool ApplyPatchBuffers(const std::vector<uint8_t> &oldBytes, const std::vector<uint8_t> &patch,
                       std::vector<uint8_t> *newBytes, std::string *error);

// ---- installing -----------------------------------------------------------

enum class StepState : uint8_t { Waiting, Working, Done, Failed, Skipped };

struct StepProgress {
	std::string id;
	std::string name;
	StepState   state = StepState::Waiting;
	std::string note;      // "Done", "Waiting", or why it failed
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

	// `chosen` is the ids to install, in the manifest's order.
	void Start(const std::string &gameDir, const std::vector<std::string> &chosen);
	void Cancel();

	bool     Running() const;
	Progress Snapshot() const;

private:
	struct Impl;
	Impl *m_impl;
};

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

} // namespace coopiii::installer
