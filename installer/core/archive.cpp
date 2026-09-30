// Zip archives: reading the ones the mods are published as, and writing the
// plain ones the tests and the release script need.
//
// The reading is miniz's. What is here is the part miniz leaves to its
// caller: refusing a path that would land outside the game folder, and
// turning a component's extract rules into a list of files to write.
#include "installer/core.h"

#include <miniz.h>

#include <cctype>
#include <cstring>

namespace coopiii::installer {
namespace {

// A path that stays where it is unpacked: relative, no drive, no "..".
bool SafePath(const std::string &path) {
	if (path.empty() || path[0] == '/' || path[0] == '\\')
		return false;
	if (path.find(':') != std::string::npos)
		return false;
	size_t start = 0;
	while (start <= path.size()) {
		size_t end = path.find_first_of("/\\", start);
		if (end == std::string::npos)
			end = path.size();
		const std::string part = path.substr(start, end - start);
		if (part == "..")
			return false;
		start = end + 1;
	}
	return true;
}

std::string Normalise(std::string path) {
	for (char &c : path)
		if (c == '\\')
			c = '/';
	return path;
}

bool IEquals(const std::string &a, const std::string &b) {
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); ++i)
		if (std::tolower(static_cast<unsigned char>(a[i])) !=
		    std::tolower(static_cast<unsigned char>(b[i])))
			return false;
	return true;
}

bool IStartsWith(const std::string &s, const std::string &prefix) {
	return s.size() >= prefix.size() && IEquals(s.substr(0, prefix.size()), prefix);
}

void PutU16(std::vector<uint8_t> &out, uint32_t v) {
	out.push_back(static_cast<uint8_t>(v & 0xFF));
	out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void PutU32(std::vector<uint8_t> &out, uint32_t v) {
	PutU16(out, v & 0xFFFF);
	PutU16(out, v >> 16);
}

} // namespace

bool ReadZip(const std::vector<uint8_t> &zip, std::vector<ArchiveEntry> *out, std::string *error) {
	out->clear();
	mz_zip_archive archive;
	std::memset(&archive, 0, sizeof(archive));
	if (zip.empty() || !mz_zip_reader_init_mem(&archive, zip.data(), zip.size(), 0)) {
		if (error)
			*error = "the download is not a zip archive";
		return false;
	}

	bool           ok    = true;
	const mz_uint  count = mz_zip_reader_get_num_files(&archive);
	for (mz_uint i = 0; i < count && ok; ++i) {
		mz_zip_archive_file_stat stat;
		if (!mz_zip_reader_file_stat(&archive, i, &stat)) {
			ok = false;
			if (error)
				*error = "the archive's directory is damaged";
			break;
		}
		if (stat.m_is_directory)
			continue;
		const std::string path = Normalise(stat.m_filename);
		if (!SafePath(path)) {
			ok = false;
			if (error)
				*error = "the archive holds a file that would land outside the game folder: " + path;
			break;
		}
		if (!stat.m_is_supported) {
			ok = false;
			if (error)
				*error = "the archive uses a kind of compression the Setup cannot read: " + path;
			break;
		}
		ArchiveEntry entry;
		entry.path = path;
		entry.bytes.resize(static_cast<size_t>(stat.m_uncomp_size));
		if (!mz_zip_reader_extract_to_mem(&archive, i, entry.bytes.empty() ? nullptr : entry.bytes.data(),
		                                  entry.bytes.size(), 0)) {
			ok = false;
			if (error)
				*error = "could not unpack " + path + " from the archive";
			break;
		}
		out->push_back(std::move(entry));
	}
	mz_zip_reader_end(&archive);
	if (!ok)
		out->clear();
	return ok;
}

bool PlanExtract(const std::vector<ArchiveEntry> &entries, const std::vector<ExtractRule> &rules,
                 std::vector<std::pair<std::string, const ArchiveEntry *>> *plan,
                 std::string *error) {
	plan->clear();
	for (const ExtractRule &rule : rules) {
		const std::string from = Normalise(rule.from);
		const std::string to   = Normalise(rule.to);
		if (!SafePath(to.empty() ? std::string("x") : to)) {
			if (error)
				*error = "the manifest points a file outside the game folder: " + rule.to;
			return false;
		}

		const bool folder  = from.empty() || from.back() == '/';
		bool       matched = false;
		for (const ArchiveEntry &e : entries) {
			if (folder) {
				if (!IStartsWith(e.path, from))
					continue;
				std::string dest = to;
				if (!dest.empty() && dest.back() != '/')
					dest += '/';
				plan->emplace_back(dest + e.path.substr(from.size()), &e);
				matched = true;
			} else if (IEquals(e.path, from)) {
				std::string dest = to.empty() ? from.substr(from.find_last_of('/') + 1) : to;
				if (dest.back() == '/')
					dest += from.substr(from.find_last_of('/') + 1);
				plan->emplace_back(dest, &e);
				matched = true;
			}
		}
		if (!matched) {
			if (error)
				*error = "the archive has no " + (from.empty() ? std::string("files") : from) +
				         ", so it is not the release the Setup expects";
			return false;
		}
	}
	return true;
}

std::vector<uint8_t> WriteZip(const std::vector<ArchiveEntry> &entries) {
	// Stored entries, one local header each, then the central directory. The
	// CRC is the one zip requires, from miniz.
	std::vector<uint8_t>  out;
	std::vector<uint32_t> offsets;
	std::vector<uint32_t> crcs;

	for (const ArchiveEntry &e : entries) {
		const uint32_t crc = static_cast<uint32_t>(
		    mz_crc32(MZ_CRC32_INIT, e.bytes.empty() ? nullptr : e.bytes.data(), e.bytes.size()));
		offsets.push_back(static_cast<uint32_t>(out.size()));
		crcs.push_back(crc);
		PutU32(out, 0x04034b50);
		PutU16(out, 20);   // version needed
		PutU16(out, 0);    // flags
		PutU16(out, 0);    // stored
		PutU16(out, 0);    // time
		PutU16(out, 0x21); // date: 1980-01-01
		PutU32(out, crc);
		PutU32(out, static_cast<uint32_t>(e.bytes.size()));
		PutU32(out, static_cast<uint32_t>(e.bytes.size()));
		PutU16(out, static_cast<uint32_t>(e.path.size()));
		PutU16(out, 0);
		out.insert(out.end(), e.path.begin(), e.path.end());
		out.insert(out.end(), e.bytes.begin(), e.bytes.end());
	}

	const uint32_t directory = static_cast<uint32_t>(out.size());
	for (size_t i = 0; i < entries.size(); ++i) {
		const ArchiveEntry &e = entries[i];
		PutU32(out, 0x02014b50);
		PutU16(out, 20);
		PutU16(out, 20);
		PutU16(out, 0);
		PutU16(out, 0);
		PutU16(out, 0);
		PutU16(out, 0x21);
		PutU32(out, crcs[i]);
		PutU32(out, static_cast<uint32_t>(e.bytes.size()));
		PutU32(out, static_cast<uint32_t>(e.bytes.size()));
		PutU16(out, static_cast<uint32_t>(e.path.size()));
		PutU16(out, 0);
		PutU16(out, 0);
		PutU16(out, 0);
		PutU16(out, 0);
		PutU32(out, 0);
		PutU32(out, offsets[i]);
		out.insert(out.end(), e.path.begin(), e.path.end());
	}
	const uint32_t directorySize = static_cast<uint32_t>(out.size()) - directory;

	PutU32(out, 0x06054b50);
	PutU16(out, 0);
	PutU16(out, 0);
	PutU16(out, static_cast<uint32_t>(entries.size()));
	PutU16(out, static_cast<uint32_t>(entries.size()));
	PutU32(out, directorySize);
	PutU32(out, directory);
	PutU16(out, 0);
	return out;
}

} // namespace coopiii::installer
