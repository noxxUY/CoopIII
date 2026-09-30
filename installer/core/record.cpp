// CoopIII-Setup.record, and Uninstall, which is what it is for.
//
// The record is plain text, one line per thing the Setup did to the game
// folder, so a player (or whoever is helping them) can read exactly what was
// changed without any tool:
//
//   CoopIII-Setup record 1
//   file <TAB> path <TAB> sha256 written <TAB> backup of what was there, or empty
//   dir  <TAB> path                                  a folder the Setup created
//   exe  <TAB> gta3.exe <TAB> MD5 before <TAB> backup of the original
//
// Paths are relative to the game folder.
#include "installer/core.h"

#include "launcher/core.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>

namespace coopiii::installer {
namespace {

constexpr const char *kHeader = "CoopIII-Setup record 1";

std::vector<std::string> SplitTabs(const std::string &line) {
	std::vector<std::string> out;
	size_t                   start = 0;
	for (;;) {
		const size_t tab = line.find('\t', start);
		out.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
		if (tab == std::string::npos)
			break;
		start = tab + 1;
	}
	return out;
}

bool SamePath(const std::string &a, const std::string &b) {
	return _stricmp(a.c_str(), b.c_str()) == 0;
}

// Removes `dir` and then each parent up to (not including) `stop`, for as long
// as they are empty.
void PruneEmpty(const std::string &dir, const std::string &stop) {
	std::string at = dir;
	while (at.size() > stop.size() && RemoveDirectoryA(at.c_str())) {
		const size_t slash = at.find_last_of("\\/");
		if (slash == std::string::npos)
			break;
		at.resize(slash);
	}
}

std::string ParentOf(const std::string &path) {
	const size_t slash = path.find_last_of("\\/");
	return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

} // namespace

bool InstallRecord::Load(const std::string &gameDir, std::string *error) {
	entries.clear();
	const std::string path = launcher::Join(gameDir, kRecordName);
	if (!launcher::FileExists(path))
		return true;   // nothing installed yet is not an error

	std::vector<uint8_t> bytes;
	if (!ReadWholeFile(path, &bytes, error))
		return false;
	const std::string text(bytes.begin(), bytes.end());

	size_t start = 0;
	bool   first = true;
	while (start < text.size()) {
		size_t end = text.find('\n', start);
		if (end == std::string::npos)
			end = text.size();
		std::string line = text.substr(start, end - start);
		start            = end + 1;
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (first) {
			first = false;
			if (line != kHeader) {
				if (error)
					*error = std::string(kRecordName) + " is not a record this Setup can read";
				return false;
			}
			continue;
		}
		if (line.empty())
			continue;
		const std::vector<std::string> f = SplitTabs(line);
		RecordEntry                    e;
		if (f[0] == "file" && f.size() == 4)
			e.kind = RecordEntry::Kind::File;
		else if (f[0] == "dir" && f.size() >= 2)
			e.kind = RecordEntry::Kind::Dir;
		else if (f[0] == "exe" && f.size() == 4)
			e.kind = RecordEntry::Kind::Exe;
		else
			continue;   // a line a newer Setup wrote; leave it be
		e.path = f[1];
		if (f.size() == 4) {
			e.hash   = f[2];
			e.backup = f[3];
		}
		entries.push_back(std::move(e));
	}
	return true;
}

bool InstallRecord::Save(const std::string &gameDir, std::string *error) const {
	const std::string path = launcher::Join(gameDir, kRecordName);
	if (entries.empty()) {
		DeleteFileA(path.c_str());
		return true;
	}
	std::string text = std::string(kHeader) + "\r\n";
	for (const RecordEntry &e : entries) {
		switch (e.kind) {
		case RecordEntry::Kind::File:
			text += "file\t" + e.path + "\t" + e.hash + "\t" + e.backup + "\r\n";
			break;
		case RecordEntry::Kind::Dir:
			text += "dir\t" + e.path + "\r\n";
			break;
		case RecordEntry::Kind::Exe:
			text += "exe\t" + e.path + "\t" + e.hash + "\t" + e.backup + "\r\n";
			break;
		}
	}
	return WriteFileAtomic(path, std::vector<uint8_t>(text.begin(), text.end()), error);
}

RecordEntry *InstallRecord::Find(RecordEntry::Kind kind, const std::string &path) {
	for (RecordEntry &e : entries)
		if (e.kind == kind && SamePath(e.path, path))
			return &e;
	return nullptr;
}

bool HasInstallRecord(const std::string &gameDir) {
	return !gameDir.empty() && launcher::FileExists(launcher::Join(gameDir, kRecordName));
}

bool Uninstall(const std::string &gameDir, std::vector<std::string> *log, std::string *error) {
	auto say = [&](const std::string &line) {
		if (log)
			log->push_back(line);
	};

	InstallRecord record;
	if (!record.Load(gameDir, error))
		return false;
	if (record.entries.empty()) {
		if (error)
			*error = "the Setup has not installed anything in this folder";
		return false;
	}
	if (GameIsRunning(gameDir)) {
		if (error)
			*error = "GTA III is running. Close it and try again.";
		return false;
	}

	std::vector<RecordEntry> kept;   // what could not be undone, for a second try
	std::vector<std::string> dirs;

	for (auto it = record.entries.rbegin(); it != record.entries.rend(); ++it) {
		const RecordEntry &e    = *it;
		const std::string  full = launcher::Join(gameDir, e.path.c_str());

		if (e.kind == RecordEntry::Kind::Dir) {
			dirs.push_back(full);
			continue;
		}

		if (e.kind == RecordEntry::Kind::Exe) {
			const std::string backup = launcher::Join(gameDir, e.backup.c_str());
			const std::string now    = launcher::Md5File(full);
			if (now == e.hash) {
				say("gta3.exe is already the build it was before the downgrade");
				DeleteFileA(backup.c_str());
				continue;
			}
			if (now != launcher::GAME_MD5) {
				// Steam or the player replaced it since. Theirs to keep.
				say("left gta3.exe alone: it has changed since the downgrade. The original is "
				    "still in " + e.backup);
				continue;
			}
			std::vector<uint8_t> original;
			std::string          why;
			if (!launcher::FileExists(backup) || !ReadWholeFile(backup, &original, &why) ||
			    launcher::Md5Buffer(original.data(), original.size()) != e.hash) {
				say("could not restore gta3.exe: " + e.backup +
				    " is missing or is not the original any more");
				kept.push_back(e);
				continue;
			}
			if (!WriteFileAtomic(full, original, &why)) {
				say("could not restore gta3.exe: " + why);
				kept.push_back(e);
				continue;
			}
			DeleteFileA(backup.c_str());
			say("put the original gta3.exe back");
			continue;
		}

		// A file.
		const bool present = launcher::FileExists(full);
		if (present && Sha256File(full) != e.hash) {
			say("left " + e.path + " alone: it has changed since the Setup wrote it" +
			    (e.backup.empty() ? std::string() : ". What was there before is in " + e.backup));
			continue;
		}
		if (present && !DeleteFileA(full.c_str())) {
			say("could not remove " + e.path + " (error " + std::to_string(GetLastError()) + ")");
			kept.push_back(e);
			continue;
		}
		if (!e.backup.empty()) {
			const std::string backup = launcher::Join(gameDir, e.backup.c_str());
			if (launcher::FileExists(backup)) {
				if (!MoveFileExA(backup.c_str(), full.c_str(), MOVEFILE_REPLACE_EXISTING)) {
					say("could not put back " + e.path + " from " + e.backup);
					kept.push_back(e);
					continue;
				}
				PruneEmpty(ParentOf(backup), gameDir);
				say("put back " + e.path);
				continue;
			}
			say("removed " + e.path + ", but the copy it replaced is gone from " + e.backup);
			continue;
		}
		say("removed " + e.path);
	}

	// Folders last, deepest first, and only the empty ones: anything the player
	// has put in a folder the Setup made keeps it alive.
	std::sort(dirs.begin(), dirs.end(),
	          [](const std::string &a, const std::string &b) { return a.size() > b.size(); });
	for (const std::string &d : dirs)
		RemoveDirectoryA(d.c_str());
	RemoveDirectoryA(launcher::Join(gameDir, kBackupDir).c_str());

	std::reverse(kept.begin(), kept.end());
	InstallRecord rest;
	rest.entries = kept;
	std::string saveError;
	rest.Save(gameDir, &saveError);

	if (!kept.empty()) {
		if (error)
			*error = "some of it could not be undone; the details say what. Running the "
			         "uninstall again will retry just those.";
		return false;
	}
	return true;
}

} // namespace coopiii::installer
