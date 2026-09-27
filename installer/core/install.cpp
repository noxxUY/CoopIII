// Doing the install: the downgrade, the downloads, and putting every file
// where it goes while writing down what was done.
//
// On a thread of its own, because it is file IO and network and neither
// belongs on a frame loop. The window reads a snapshot under a lock and draws
// whatever it finds; nothing it draws can block.
//
// Three rules hold for every file this writes into a game folder:
//
//   - it is written whole or not at all (WriteFileAtomic);
//   - a file that was already there and is not the Setup's own is moved into
//     CoopIII-Setup-backup first, so Uninstall can put it back;
//   - an .ini that is already there is left alone. It holds somebody's
//     settings, and the one the archive carries is only the defaults.
#include "installer/core.h"

#include "launcher/core.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#pragma comment(lib, "ole32.lib")

namespace coopiii::installer {
namespace {

std::wstring Widen(const std::string &utf8) {
	if (utf8.empty())
		return std::wstring();
	const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
	std::wstring out(n > 0 ? n - 1 : 0, L'\0');
	if (n > 1)
		MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, out.data(), n);
	return out;
}

std::string Backslashes(std::string path) {
	for (char &c : path)
		if (c == '/')
			c = '\\';
	while (!path.empty() && path.back() == '\\')
		path.pop_back();
	return path;
}

std::string JoinRel(const std::string &dir, const std::string &leaf) {
	if (dir.empty())
		return Backslashes(leaf);
	return Backslashes(dir) + "\\" + Backslashes(leaf);
}

bool EndsWithI(const std::string &s, const char *suffix) {
	const size_t n = std::strlen(suffix);
	return s.size() >= n && _stricmp(s.c_str() + s.size() - n, suffix) == 0;
}

// The name a download should be saved as: the last path segment of its URL.
std::string LeafOf(const std::string &url) {
	const size_t slash = url.find_last_of('/');
	std::string  leaf  = slash == std::string::npos ? url : url.substr(slash + 1);
	const size_t query = leaf.find('?');
	if (query != std::string::npos)
		leaf.resize(query);
	return leaf.empty() ? std::string("download.bin") : leaf;
}

std::string Megabytes(uint64_t bytes) {
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%.1f", bytes / (1024.0 * 1024.0));
	return buf;
}

PayloadReader g_payload;

// The Setup's own copy of a file, or the one beside it.
bool ReadPayload(const std::string &name, std::vector<uint8_t> *out) {
	if (g_payload && g_payload(name, out))
		return true;
	const std::string beside = launcher::Join(SetupDir(), name.c_str());
	return launcher::FileExists(beside) && ReadWholeFile(beside, out, nullptr) && !out->empty();
}

// A *.c3patch beside the Setup that turns this exact exe into v1.0.
std::string FindLocalPatch(const std::string &md5) {
	WIN32_FIND_DATAA  data;
	const std::string dir  = SetupDir();
	HANDLE            find = FindFirstFileA(launcher::Join(dir, "*.c3patch").c_str(), &data);
	if (find == INVALID_HANDLE_VALUE)
		return std::string();
	std::string found;
	do {
		const std::string path = launcher::Join(dir, data.cFileName);
		PatchInfo         info;
		if (ReadPatchInfo(path, &info, nullptr) && info.oldMd5 == md5 &&
		    info.newMd5 == launcher::GAME_MD5) {
			found = path;
			break;
		}
	} while (FindNextFileA(find, &data));
	FindClose(find);
	return found;
}

bool CanWriteTo(const std::string &dir) {
	const std::string probe = launcher::Join(dir, "coopiii-setup-write-test.tmp");
	HANDLE h = CreateFileA(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
	                       FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return false;
	CloseHandle(h);
	return true;
}

} // namespace

void SetPayloadReader(PayloadReader reader) { g_payload = std::move(reader); }

// ---- the game folder ------------------------------------------------------

bool GameIsRunning(const std::string &gameDir) {
	const std::string exe = launcher::Join(gameDir, "gta3.exe");
	// An exe that is running is mapped as an image, and Windows refuses to
	// open a mapped image for writing with a sharing violation. Nothing is
	// written; the handle is closed at once.
	HANDLE h = CreateFileA(exe.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE |
	                                                       FILE_SHARE_DELETE,
	                       nullptr, OPEN_EXISTING, 0, nullptr);
	if (h != INVALID_HANDLE_VALUE) {
		CloseHandle(h);
		return false;
	}
	return GetLastError() == ERROR_SHARING_VIOLATION;
}

GameExe InspectGame(const std::string &gameDir, const Manifest &manifest) {
	GameExe           g;
	const std::string exe = launcher::Join(gameDir, "gta3.exe");
	if (gameDir.empty() || !launcher::FileExists(exe))
		return g;
	g.present = true;
	g.md5     = launcher::Md5File(exe);
	WIN32_FILE_ATTRIBUTE_DATA d;
	if (GetFileAttributesExA(exe.c_str(), GetFileExInfoStandard, &d))
		g.size = (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
	g.isV10   = g.md5 == launcher::GAME_MD5;
	g.running = GameIsRunning(gameDir);
	if (const Build *b = manifest.FindBuild(g.md5))
		g.buildName = b->name;
	if (!g.isV10 && !g.md5.empty()) {
		g.localPatch = FindLocalPatch(g.md5);
		g.knownPatch = manifest.FindPatch(g.md5);
	}
	return g;
}

// ---- the job --------------------------------------------------------------

struct InstallJob::Impl {
	mutable std::mutex mutex;
	Progress           progress;
	std::thread        worker;
	std::atomic<bool>  cancel{false};

	InstallOptions options;
	const Manifest *manifest = nullptr;
	InstallRecord   record;

	void Detail(const char *prefix, const std::string &text) {
		std::lock_guard<std::mutex> lock(mutex);
		progress.details.push_back("[ " + std::string(prefix) + " ] " + text);
	}

	void SetStep(size_t index, StepState state, const std::string &note) {
		std::lock_guard<std::mutex> lock(mutex);
		if (index < progress.steps.size()) {
			progress.steps[index].state = state;
			progress.steps[index].note  = note;
		}
	}

	void SetNote(size_t index, const std::string &note) {
		std::lock_guard<std::mutex> lock(mutex);
		if (index < progress.steps.size())
			progress.steps[index].note = note;
	}

	bool Cancelled() const { return cancel.load(); }

	bool SaveRecord(std::string *error) { return record.Save(options.gameDir, error); }

	// Creates each missing folder on the way to `relDir`, writing each one down.
	bool MakeDirs(const std::string &relDir, std::string *error) {
		if (relDir.empty())
			return true;
		size_t at = 0;
		for (;;) {
			const size_t slash = relDir.find('\\', at);
			const std::string part = relDir.substr(0, slash);
			const std::string full = launcher::Join(options.gameDir, part.c_str());
			if (!launcher::DirExists(full)) {
				if (!CreateDirectoryA(full.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
					if (error)
						*error = "could not create the folder " + part;
					return false;
				}
				if (!record.Find(RecordEntry::Kind::Dir, part))
					record.entries.push_back({RecordEntry::Kind::Dir, part, "", ""});
			}
			if (slash == std::string::npos)
				return true;
			at = slash + 1;
		}
	}

	// Puts one file into the game folder, by the three rules at the top.
	bool Place(const std::string &relIn, const std::vector<uint8_t> &bytes, std::string *error) {
		const std::string rel  = Backslashes(relIn);
		const std::string full = launcher::Join(options.gameDir, rel.c_str());
		const std::string hash = Sha256(bytes.data(), bytes.size());

		const size_t slash = rel.find_last_of('\\');
		if (slash != std::string::npos && !MakeDirs(rel.substr(0, slash), error))
			return false;

		RecordEntry *ours = record.Find(RecordEntry::Kind::File, rel);
		std::string  backup;

		if (launcher::FileExists(full)) {
			const std::string now = Sha256File(full);
			if (now == hash)
				return true;   // already exactly this; whoever put it there keeps it
			if (EndsWithI(rel, ".ini") && !(ours && ours->hash == now)) {
				Detail("..", "kept your " + rel);
				return true;
			}
			if (!ours) {
				// Somebody else's file. Out of the way, not overwritten.
				backup = JoinRel(kBackupDir, rel);
				const std::string backupFull = launcher::Join(options.gameDir, backup.c_str());
				const size_t      cut        = backupFull.find_last_of('\\');
				if (!EnsureDir(backupFull.substr(0, cut)) ||
				    !CopyFileA(full.c_str(), backupFull.c_str(), FALSE)) {
					if (error)
						*error = "could not back up " + rel + " before replacing it";
					return false;
				}
			} else if (ours->hash != now) {
				// Ours, changed since. The backup slot already holds what was
				// there before the first install, and that is what Uninstall
				// owes the player, so this one is not kept.
				Detail("..", "replaced " + rel + ", which had changed since the last install");
			}
		}

		if (!WriteFileAtomic(full, bytes, error))
			return false;

		if (ours) {
			ours->hash = hash;
		} else {
			record.entries.push_back({RecordEntry::Kind::File, rel, hash, backup});
		}
		return true;
	}

	bool Downgrade(size_t step, std::string *error);
	bool Install(size_t step, const Component &c, std::string *error);
	void Run();
};

bool InstallJob::Impl::Downgrade(size_t step, std::string *error) {
	const std::string exe = launcher::Join(options.gameDir, "gta3.exe");
	const GameExe     g   = InspectGame(options.gameDir, *manifest);
	if (!g.present) {
		*error = "there is no gta3.exe in the game folder";
		return false;
	}
	if (g.isV10) {
		Detail("ok", "gta3.exe is already v1.0 retail");
		return true;
	}

	std::string patch = g.localPatch;
	if (patch.empty() && g.knownPatch) {
		const PatchSource &p = *g.knownPatch;
		Detail("..", "downloading the downgrade patch for the " +
		                 (p.name.empty() ? std::string("detected") : p.name) + " build");
		std::vector<std::string> sources;
		if (!p.url.empty())
			sources.push_back(p.url);
		sources.insert(sources.end(), p.mirrors.begin(), p.mirrors.end());
		const std::string cache = options.cacheDir.empty() ? DefaultCacheDir() : options.cacheDir;
		EnsureDir(cache);
		patch = launcher::Join(cache, p.file.c_str());
		if (!FetchVerified(
		        sources, p.sha256, patch,
		        [&](uint64_t done, uint64_t total) {
			        SetNote(step, Megabytes(done) + (total ? " of " + Megabytes(total) : "") + " MB");
		        },
		        [&] { return Cancelled(); }, [&](const std::string &line) { Detail("..", line); },
		        error))
			return false;
	}
	if (patch.empty()) {
		char size[32];
		std::snprintf(size, sizeof(size), "%llu", static_cast<unsigned long long>(g.size));
		*error = "no downgrade patch is known for this gta3.exe (" +
		         (g.buildName.empty() ? std::string("unrecognised build") : g.buildName) +
		         ", MD5 " + g.md5 + ", " + size + " bytes)";
		return false;
	}

	PatchInfo info;
	if (!ReadPatchInfo(patch, &info, error))
		return false;
	if (info.newMd5 != launcher::GAME_MD5) {
		*error = "that patch does not produce v1.0 retail";
		return false;
	}

	// The original goes to gta3.exe.bak, unless something else already lives
	// there, in which case it gets a name of its own rather than evicting it.
	std::string backup = "gta3.exe.bak";
	{
		const std::string at = launcher::Join(options.gameDir, backup.c_str());
		if (launcher::FileExists(at) && launcher::Md5File(at) != g.md5)
			backup = "gta3.exe." + g.md5.substr(0, 8) + ".bak";
	}
	Detail("..", "patching gta3.exe; the original is kept as " + backup);
	if (!ApplyPatch(exe, patch, launcher::Join(options.gameDir, backup.c_str()), error))
		return false;

	if (RecordEntry *e = record.Find(RecordEntry::Kind::Exe, "gta3.exe")) {
		e->hash   = g.md5;
		e->backup = backup;
	} else {
		record.entries.push_back({RecordEntry::Kind::Exe, "gta3.exe", g.md5, backup});
	}
	if (!SaveRecord(error))
		return false;

	{
		std::lock_guard<std::mutex> lock(mutex);
		progress.downgraded = true;
		progress.backup     = backup;
	}
	Detail("ok", "gta3.exe is now v1.0 retail");
	return true;
}

bool InstallJob::Impl::Install(size_t step, const Component &c, std::string *error) {
	if (c.source == "local") {
		Detail("..", c.name + ": writing files into the game folder");
		for (const std::string &file : c.files) {
			std::vector<uint8_t> bytes;
			if (!ReadPayload(file, &bytes)) {
				*error = file + " is not in this Setup";
				return false;
			}
			if (!Place(JoinRel(c.destination, file), bytes, error))
				return false;
		}
	} else {
		if (!c.Ready()) {
			*error = "no download is set for this component yet";
			return false;
		}
		const std::string cache = options.cacheDir.empty() ? DefaultCacheDir() : options.cacheDir;
		if (!EnsureDir(cache)) {
			*error = "could not create the download folder " + cache;
			return false;
		}
		const std::string leaf =
		    LeafOf(c.url.empty() ? c.mirrors.front() : c.url);
		const std::string saved = launcher::Join(cache, (c.sha256.substr(0, 12) + "-" + leaf).c_str());

		Detail("..", c.name + ": downloading " + leaf);
		if (!FetchVerified(
		        c.Sources(), c.sha256, saved,
		        [&](uint64_t done, uint64_t total) {
			        SetNote(step, Megabytes(done) + (total ? " of " + Megabytes(total) : "") + " MB");
		        },
		        [&] { return Cancelled(); }, [&](const std::string &line) { Detail("..", line); },
		        error))
			return false;
		SetNote(step, "Installing");

		std::vector<uint8_t> bytes;
		if (!ReadWholeFile(saved, &bytes, error))
			return false;

		if (c.archive == "zip") {
			std::vector<ArchiveEntry>                                entries;
			std::vector<std::pair<std::string, const ArchiveEntry *>> plan;
			if (!ReadZip(bytes, &entries, error) || !PlanExtract(entries, c.extract, &plan, error))
				return false;
			for (const auto &item : plan)
				if (!Place(item.first, item.second->bytes, error))
					return false;
		} else if (!Place(JoinRel(c.destination, c.saveAs.empty() ? leaf : c.saveAs), bytes, error)) {
			return false;
		}
	}

	for (const DefaultFile &d : c.defaults) {
		const std::string rel = Backslashes(d.to);
		if (launcher::FileExists(launcher::Join(options.gameDir, rel.c_str())))
			continue;
		std::vector<uint8_t> bytes;
		if (!ReadPayload(d.file, &bytes)) {
			*error = d.file + " is not in this Setup";
			return false;
		}
		if (!Place(rel, bytes, error))
			return false;
	}
	return SaveRecord(error);
}

void InstallJob::Impl::Run() {
	std::string error;
	bool        stop = false;

	if (!record.Load(options.gameDir, &error)) {
		stop = true;
	} else if (!launcher::DirExists(options.gameDir)) {
		error = "the game folder does not exist";
		stop  = true;
	} else if (GameIsRunning(options.gameDir)) {
		error = "GTA III is running. Close it and try again.";
		stop  = true;
	} else if (!CanWriteTo(options.gameDir)) {
		error = "the Setup cannot write into the game folder. Run it as administrator, or "
		        "choose a copy of the game you own the folder of.";
		stop  = true;
	}
	if (stop) {
		Detail("!!", error);
		std::lock_guard<std::mutex> lock(mutex);
		for (StepProgress &s : progress.steps) {
			s.state = StepState::Failed;
			s.note  = error;
		}
		progress.failed  = true;
		progress.running = false;
		return;
	}

	std::string blocked;   // set once a failure makes the rest pointless
	for (size_t i = 0; i < progress.steps.size(); ++i) {
		const std::string id = progress.steps[i].id;
		if (Cancelled()) {
			SetStep(i, StepState::Skipped, "Cancelled");
			continue;
		}
		if (!blocked.empty()) {
			SetStep(i, StepState::Skipped, blocked);
			continue;
		}

		SetStep(i, StepState::Working, "Installing");
		error.clear();
		bool ok = false;
		if (id == kDowngradeId) {
			ok = Downgrade(i, &error);
			if (!ok)
				blocked = "Skipped: the downgrade did not finish";
		} else if (const Component *c = manifest->Find(id)) {
			ok = Install(i, *c, &error);
			// What was placed before a failure is still written down.
			std::string ignored;
			SaveRecord(&ignored);
			if (!ok && c->required && c->id == "coopiii")
				blocked = "Skipped: CoopIII itself did not install";
		}

		const std::string name = progress.steps[i].name;
		if (ok) {
			SetStep(i, StepState::Done, "Done");
			Detail("ok", name + " installed");
			std::lock_guard<std::mutex> lock(mutex);
			++progress.finished;
		} else if (Cancelled()) {
			SetStep(i, StepState::Skipped, "Cancelled");
			Detail("..", name + ": cancelled");
		} else {
			SetStep(i, StepState::Failed, error.empty() ? "Failed" : error);
			Detail("!!", name + ": " + (error.empty() ? "failed" : error));
			std::lock_guard<std::mutex> lock(mutex);
			progress.failed = true;
		}
	}

	std::lock_guard<std::mutex> lock(mutex);
	progress.running = false;
}

InstallJob::InstallJob() : m_impl(new Impl()) {}

InstallJob::~InstallJob() {
	Cancel();
	if (m_impl->worker.joinable())
		m_impl->worker.join();
	delete m_impl;
}

void InstallJob::Start(const InstallOptions &options) {
	if (Running())
		return;
	if (m_impl->worker.joinable())
		m_impl->worker.join();

	m_impl->options  = options;
	m_impl->manifest = options.manifest ? options.manifest : &BuiltInManifest();

	{
		std::lock_guard<std::mutex> lock(m_impl->mutex);
		m_impl->progress = Progress{};
		if (options.downgrade)
			m_impl->progress.steps.push_back(
			    {kDowngradeId, "Downgrade to v1.0", StepState::Waiting, "Waiting"});
		// The manifest's order, whatever order the ids came in.
		for (const Component &c : m_impl->manifest->components)
			for (const std::string &id : options.components)
				if (id == c.id) {
					m_impl->progress.steps.push_back({c.id, c.name, StepState::Waiting, "Waiting"});
					break;
				}
		m_impl->progress.total   = static_cast<int>(m_impl->progress.steps.size());
		m_impl->progress.running = true;
	}
	m_impl->cancel.store(false);

	Impl *impl     = m_impl;
	m_impl->worker = std::thread([impl] { impl->Run(); });
}

void InstallJob::Cancel() { m_impl->cancel.store(true); }

bool InstallJob::Running() const {
	std::lock_guard<std::mutex> lock(m_impl->mutex);
	return m_impl->progress.running;
}

Progress InstallJob::Snapshot() const {
	std::lock_guard<std::mutex> lock(m_impl->mutex);
	Progress copy  = m_impl->progress;
	copy.cancelled = m_impl->cancel.load();
	return copy;
}

// ---- odds and ends --------------------------------------------------------

std::string SetupDir() { return launcher::ExeDir(); }

bool CreateDesktopShortcut(const std::string &target, const std::string &name,
                           const std::string &workingDir) {
	const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	if (FAILED(init) && init != RPC_E_CHANGED_MODE)
		return false;

	bool ok = false;
	IShellLinkW *link = nullptr;
	if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
	                               IID_PPV_ARGS(&link))) &&
	    link) {
		link->SetPath(Widen(target).c_str());
		link->SetWorkingDirectory(Widen(workingDir).c_str());
		link->SetDescription(L"Start GTA III with CoopIII");

		IPersistFile *file = nullptr;
		if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))) && file) {
			PWSTR desktop = nullptr;
			if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop)) &&
			    desktop) {
				const std::wstring path = std::wstring(desktop) + L"\\" + Widen(name) + L".lnk";
				ok                      = SUCCEEDED(file->Save(path.c_str(), TRUE));
				CoTaskMemFree(desktop);
			}
			file->Release();
		}
		link->Release();
	}

	if (SUCCEEDED(init))
		CoUninitialize();
	return ok;
}

} // namespace coopiii::installer
