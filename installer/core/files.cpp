// Reading and writing whole files, the way every piece of the Setup needs to.
//
// A write goes to a file beside the target and is renamed over it. A player
// whose disk fills up half way through, or whose antivirus holds the file
// open, keeps the file they had rather than half of a new one.
#include "installer/core.h"

#include "launcher/core.h"

#include <windows.h>

#include <cstdio>

namespace coopiii::installer {

bool ReadWholeFile(const std::string &path, std::vector<uint8_t> *out, std::string *error) {
	FILE *fh = std::fopen(path.c_str(), "rb");
	if (!fh) {
		if (error)
			*error = "could not open " + path;
		return false;
	}
	std::fseek(fh, 0, SEEK_END);
	const long size = std::ftell(fh);
	std::fseek(fh, 0, SEEK_SET);
	out->resize(size > 0 ? static_cast<size_t>(size) : 0);
	const size_t got = out->empty() ? 0 : std::fread(out->data(), 1, out->size(), fh);
	std::fclose(fh);
	if (got != out->size()) {
		if (error)
			*error = "could not read all of " + path;
		return false;
	}
	return true;
}

bool WriteFileAtomic(const std::string &path, const std::vector<uint8_t> &bytes,
                     std::string *error) {
	const std::string temp = path + ".coopiii-new";
	FILE             *fh   = std::fopen(temp.c_str(), "wb");
	if (!fh) {
		if (error)
			*error = "could not write " + path;
		return false;
	}
	const size_t put = bytes.empty() ? 0 : std::fwrite(bytes.data(), 1, bytes.size(), fh);
	const bool   flushed = std::fflush(fh) == 0;
	std::fclose(fh);
	if (put != bytes.size() || !flushed) {
		DeleteFileA(temp.c_str());
		if (error)
			*error = "could not write all of " + path + ". Is the disk full?";
		return false;
	}
	if (!MoveFileExA(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		const DWORD why = GetLastError();
		DeleteFileA(temp.c_str());
		if (error) {
			*error = "could not replace " + path;
			if (why == ERROR_SHARING_VIOLATION || why == ERROR_ACCESS_DENIED)
				*error += ". Something has it open - is GTA III still running?";
		}
		return false;
	}
	return true;
}

bool EnsureDir(const std::string &path) {
	if (path.empty() || launcher::DirExists(path))
		return true;
	const size_t slash = path.find_last_of("\\/");
	if (slash != std::string::npos && slash > 2 && !EnsureDir(path.substr(0, slash)))
		return false;
	return CreateDirectoryA(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

} // namespace coopiii::installer
