// The downgrader's patch format.
//
// A patch is a list of "copy this run from the file you have" and "insert
// these bytes". Two builds of the same program share long runs, so the copies
// carry most of the file and only the real differences are stored. The repo
// carries no gta3.exe, and this is what lets it stay that way while the Setup
// can still turn a Steam copy into v1.0 retail.
//
// Both ends verify MD5: the patch says which build it was made from and which
// build it makes, and neither the generator nor the applier will guess.
#include "installer/core.h"

#include "launcher/core.h"

#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace coopiii::installer {
namespace {

constexpr char     kMagic[8]   = {'C', '3', 'P', 'A', 'T', 'C', 'H', '1'};
constexpr size_t   kHeaderSize = 8 + 4 + 32 + 4 + 32 + 4;
constexpr uint32_t kBlock      = 32;   // the run length the matcher indexes on

void PutU32(std::vector<uint8_t> &out, uint32_t value) {
	out.push_back(static_cast<uint8_t>(value & 0xFF));
	out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
	out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
	out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

uint32_t GetU32(const uint8_t *p) {
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
	       (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool ReadFile(const std::string &path, std::vector<uint8_t> *out, std::string *error) {
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

bool WriteFile(const std::string &path, const std::vector<uint8_t> &bytes, std::string *error) {
	FILE *fh = std::fopen(path.c_str(), "wb");
	if (!fh) {
		if (error)
			*error = "could not write " + path;
		return false;
	}
	const size_t put = bytes.empty() ? 0 : std::fwrite(bytes.data(), 1, bytes.size(), fh);
	std::fclose(fh);
	if (put != bytes.size()) {
		if (error)
			*error = "could not write all of " + path;
		return false;
	}
	return true;
}

std::string Md5Of(const std::vector<uint8_t> &bytes) {
	// launcher-core owns the MD5, and the launcher's version check and the
	// downgrader have to agree about what a build's fingerprint is.
	return launcher::Md5Buffer(bytes.data(), bytes.size());
}

// A cheap rolling key over kBlock bytes. Only used to find candidate
// positions; every candidate is confirmed byte for byte.
uint64_t KeyAt(const uint8_t *p) {
	uint64_t key = 1469598103934665603ull;
	for (uint32_t i = 0; i < kBlock; ++i) {
		key ^= p[i];
		key *= 1099511628211ull;
	}
	return key;
}

} // namespace

bool MakePatchBuffers(const std::vector<uint8_t> &oldBytes, const std::vector<uint8_t> &newBytes,
                      std::vector<uint8_t> *patch, std::string *error) {
	if (newBytes.empty()) {
		if (error)
			*error = "the new file is empty";
		return false;
	}

	// Index every kBlock-aligned run of the old file. Keeping only the first
	// position for a key is enough: a run that repeats is a run either copy
	// would reproduce.
	std::unordered_map<uint64_t, uint32_t> index;
	if (oldBytes.size() >= kBlock) {
		index.reserve(oldBytes.size() / kBlock * 2);
		for (uint32_t i = 0; i + kBlock <= oldBytes.size(); i += kBlock / 2)
			index.emplace(KeyAt(oldBytes.data() + i), i);
	}

	struct Op {
		uint8_t  kind;   // 0 copy, 1 insert
		uint32_t a, b;
		uint32_t literalAt;
	};
	std::vector<Op>      ops;
	std::vector<uint8_t> literals;

	uint32_t pos       = 0;
	uint32_t literalAt = 0;
	uint32_t pending   = 0;   // literal bytes not yet flushed

	auto flushLiteral = [&] {
		if (pending == 0)
			return;
		Op op{1, pending, 0, literalAt};
		ops.push_back(op);
		pending = 0;
	};

	while (pos < newBytes.size()) {
		bool matched = false;

		if (pos + kBlock <= newBytes.size() && !index.empty()) {
			const auto found = index.find(KeyAt(newBytes.data() + pos));
			if (found != index.end()) {
				const uint32_t candidate = found->second;
				// Confirm, then run the match as far as it goes in both
				// directions of length.
				if (candidate + kBlock <= oldBytes.size() &&
				    std::memcmp(oldBytes.data() + candidate, newBytes.data() + pos, kBlock) == 0) {
					uint32_t length = kBlock;
					while (candidate + length < oldBytes.size() && pos + length < newBytes.size() &&
					       oldBytes[candidate + length] == newBytes[pos + length])
						++length;

					flushLiteral();
					ops.push_back(Op{0, candidate, length, 0});
					pos += length;
					matched = true;
				}
			}
		}

		if (!matched) {
			if (pending == 0)
				literalAt = static_cast<uint32_t>(literals.size());
			literals.push_back(newBytes[pos]);
			++pending;
			++pos;
		}
	}
	flushLiteral();

	patch->clear();
	patch->insert(patch->end(), kMagic, kMagic + 8);
	PutU32(*patch, static_cast<uint32_t>(oldBytes.size()));
	const std::string oldMd5 = Md5Of(oldBytes);
	const std::string newMd5 = Md5Of(newBytes);
	patch->insert(patch->end(), oldMd5.begin(), oldMd5.end());
	PutU32(*patch, static_cast<uint32_t>(newBytes.size()));
	patch->insert(patch->end(), newMd5.begin(), newMd5.end());
	PutU32(*patch, static_cast<uint32_t>(ops.size()));

	for (const Op &op : ops) {
		patch->push_back(op.kind);
		if (op.kind == 0) {
			PutU32(*patch, op.a);
			PutU32(*patch, op.b);
		} else {
			PutU32(*patch, op.a);
			PutU32(*patch, 0);
			patch->insert(patch->end(), literals.begin() + op.literalAt,
			              literals.begin() + op.literalAt + op.a);
		}
	}
	return true;
}

bool ApplyPatchBuffers(const std::vector<uint8_t> &oldBytes, const std::vector<uint8_t> &patch,
                       std::vector<uint8_t> *newBytes, std::string *error) {
	auto fail = [&](const char *why) {
		if (error)
			*error = why;
		return false;
	};

	if (patch.size() < kHeaderSize || std::memcmp(patch.data(), kMagic, 8) != 0)
		return fail("that is not a CoopIII patch file");

	const uint8_t *p        = patch.data() + 8;
	const uint32_t oldSize  = GetU32(p);
	const std::string oldMd5(reinterpret_cast<const char *>(p + 4), 32);
	const uint32_t newSize  = GetU32(p + 36);
	const std::string newMd5(reinterpret_cast<const char *>(p + 40), 32);
	const uint32_t opCount  = GetU32(p + 72);

	if (oldBytes.size() != oldSize)
		return fail("this patch is for a different build: the file is the wrong size");
	if (Md5Of(oldBytes) != oldMd5)
		return fail("this patch is for a different build: the file's MD5 does not match");

	newBytes->clear();
	newBytes->reserve(newSize);

	const uint8_t *cursor = patch.data() + kHeaderSize;
	const uint8_t *end    = patch.data() + patch.size();

	for (uint32_t i = 0; i < opCount; ++i) {
		if (cursor + 9 > end)
			return fail("the patch ends in the middle of an instruction");
		const uint8_t  kind = *cursor;
		const uint32_t a    = GetU32(cursor + 1);
		const uint32_t b    = GetU32(cursor + 5);
		cursor += 9;

		if (kind == 0) {
			if (static_cast<uint64_t>(a) + b > oldBytes.size())
				return fail("the patch copies from past the end of the file");
			newBytes->insert(newBytes->end(), oldBytes.begin() + a, oldBytes.begin() + a + b);
		} else if (kind == 1) {
			if (cursor + a > end)
				return fail("the patch ends in the middle of its own data");
			newBytes->insert(newBytes->end(), cursor, cursor + a);
			cursor += a;
		} else {
			return fail("the patch has an instruction this Setup does not understand");
		}
	}

	if (newBytes->size() != newSize)
		return fail("the patch produced a file of the wrong size");
	if (Md5Of(*newBytes) != newMd5)
		return fail("the patch produced a file with the wrong MD5");
	return true;
}

bool ReadPatchInfo(const std::string &patchPath, PatchInfo *info, std::string *error) {
	std::vector<uint8_t> bytes;
	if (!ReadFile(patchPath, &bytes, error))
		return false;
	if (bytes.size() < kHeaderSize || std::memcmp(bytes.data(), kMagic, 8) != 0) {
		if (error)
			*error = "that is not a CoopIII patch file";
		return false;
	}
	const uint8_t *p = bytes.data() + 8;
	info->oldSize    = GetU32(p);
	info->oldMd5.assign(reinterpret_cast<const char *>(p + 4), 32);
	info->newSize = GetU32(p + 36);
	info->newMd5.assign(reinterpret_cast<const char *>(p + 40), 32);
	info->ops = GetU32(p + 72);
	return true;
}

bool MakePatch(const std::string &oldPath, const std::string &newPath,
               const std::string &patchPath, std::string *error) {
	std::vector<uint8_t> oldBytes, newBytes, patch;
	if (!ReadFile(oldPath, &oldBytes, error) || !ReadFile(newPath, &newBytes, error))
		return false;
	if (!MakePatchBuffers(oldBytes, newBytes, &patch, error))
		return false;
	return WriteFile(patchPath, patch, error);
}

bool ApplyPatch(const std::string &exePath, const std::string &patchPath,
                const std::string &backupPath, std::string *error) {
	std::vector<uint8_t> oldBytes, patch, newBytes;
	if (!ReadFile(exePath, &oldBytes, error) || !ReadFile(patchPath, &patch, error))
		return false;
	if (!ApplyPatchBuffers(oldBytes, patch, &newBytes, error))
		return false;

	// The original goes first. Everything after this point can fail without
	// the player losing the exe they started with.
	if (!backupPath.empty() && !WriteFile(backupPath, oldBytes, error))
		return false;
	return WriteFile(exePath, newBytes, error);
}

} // namespace coopiii::installer
