// The downgrader's patch format. core.h has the layout.
//
// The differ is Colin Percival's bsdiff algorithm (the scan loop below follows
// its structure step for step), with a hash index over 8-byte windows standing
// in for bsdiff's suffix array. Why bsdiff and not a copy/insert delta: two
// builds of one program are two compiles, so nearly every instruction that
// mentions an address differs by the distance the code moved. A copy/insert
// delta breaks its copies at every one of those and has to store the rest of
// the instruction as new bytes, which for a game executable ends up being most
// of the game. bsdiff keeps the alignment going through them and stores the
// byte-wise difference instead, which is mostly zeros and deflates to little.
//
// Both ends verify MD5: the patch says which build it was made from and which
// build it makes, and neither the generator nor the applier will guess.
#include "installer/core.h"

#include "launcher/core.h"

#include <miniz.h>

#include <algorithm>
#include <cstring>

namespace coopiii::installer {
namespace {

constexpr char     kMagic[8]   = {'C', '3', 'P', 'A', 'T', 'C', 'H', '2'};
constexpr size_t   kHeaderSize = 8 + 4 + 32 + 4 + 32 + 4 + 6 * 4;
constexpr uint32_t kWindow     = 8;          // the shortest match the index can find
constexpr int      kCandidates = 48;         // chain entries tried per position
constexpr uint32_t kHashBits   = 22;

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

std::string Md5Of(const std::vector<uint8_t> &bytes) {
	// launcher-core owns the MD5, and the launcher's version check and the
	// downgrader have to agree about what a build's fingerprint is.
	return launcher::Md5Buffer(bytes.data(), bytes.size());
}

uint32_t HashAt(const uint8_t *p) {
	uint64_t v;
	std::memcpy(&v, p, sizeof(v));
	return static_cast<uint32_t>((v * 0x9E3779B97F4A7C15ull) >> (64 - kHashBits));
}

// Where in the old file the longest exact match for new[at...] starts.
class Index {
public:
	explicit Index(const std::vector<uint8_t> &old) : m_old(old) {
		m_head.assign(size_t(1) << kHashBits, 0);
		if (old.size() >= kWindow) {
			m_next.assign(old.size(), 0);
			for (uint32_t i = 0; i + kWindow <= old.size(); ++i) {
				const uint32_t h = HashAt(old.data() + i);
				m_next[i]        = m_head[h];
				m_head[h]        = i + 1;
			}
		}
	}

	int64_t Search(const std::vector<uint8_t> &nu, int64_t at, int64_t *pos) const {
		*pos = 0;
		if (m_next.empty() || at + kWindow > static_cast<int64_t>(nu.size()))
			return 0;
		const uint8_t *n     = nu.data() + at;
		const int64_t  limit = static_cast<int64_t>(nu.size()) - at;
		int64_t        best  = 0;
		uint32_t       link  = m_head[HashAt(n)];
		for (int tries = 0; link != 0 && tries < kCandidates; ++tries) {
			const uint32_t c    = link - 1;
			const int64_t  room = std::min<int64_t>(limit, static_cast<int64_t>(m_old.size()) - c);
			int64_t        len  = 0;
			while (len < room && m_old[c + len] == n[len])
				++len;
			if (len > best) {
				best = len;
				*pos = c;
				if (len >= 1 << 16)
					break;   // long enough that nothing will beat it usefully
			}
			link = m_next[c];
		}
		return best >= kWindow ? best : 0;
	}

private:
	const std::vector<uint8_t> &m_old;
	std::vector<uint32_t>       m_head;
	std::vector<uint32_t>       m_next;
};

bool Deflate(const std::vector<uint8_t> &raw, std::vector<uint8_t> *out) {
	mz_ulong bound = mz_compressBound(static_cast<mz_ulong>(raw.size()));
	out->resize(bound);
	if (mz_compress2(out->data(), &bound, raw.empty() ? nullptr : raw.data(),
	                 static_cast<mz_ulong>(raw.size()), MZ_UBER_COMPRESSION) != MZ_OK)
		return false;
	out->resize(bound);
	return true;
}

bool Inflate(const uint8_t *packed, size_t packedSize, size_t rawSize, std::vector<uint8_t> *out) {
	out->resize(rawSize);
	if (rawSize == 0)
		return true;
	mz_ulong got = static_cast<mz_ulong>(rawSize);
	if (mz_uncompress(out->data(), &got, packed, static_cast<mz_ulong>(packedSize)) != MZ_OK)
		return false;
	return got == rawSize;
}

struct Header {
	uint32_t    oldSize = 0, newSize = 0, controls = 0;
	std::string oldMd5, newMd5;
	uint32_t    raw[3]    = {0, 0, 0};
	uint32_t    packed[3] = {0, 0, 0};
};

bool ParseHeader(const std::vector<uint8_t> &patch, Header *h, std::string *error) {
	if (patch.size() < kHeaderSize || std::memcmp(patch.data(), kMagic, 8) != 0) {
		if (error)
			*error = "that is not a CoopIII downgrade patch";
		return false;
	}
	const uint8_t *p = patch.data() + 8;
	h->oldSize       = GetU32(p);
	h->oldMd5.assign(reinterpret_cast<const char *>(p + 4), 32);
	h->newSize = GetU32(p + 36);
	h->newMd5.assign(reinterpret_cast<const char *>(p + 40), 32);
	h->controls = GetU32(p + 72);
	uint64_t body = 0;
	for (int i = 0; i < 3; ++i) {
		h->raw[i]    = GetU32(p + 76 + i * 8);
		h->packed[i] = GetU32(p + 80 + i * 8);
		body += h->packed[i];
	}
	if (kHeaderSize + body != patch.size()) {
		if (error)
			*error = "the patch file is incomplete or damaged";
		return false;
	}
	if (static_cast<uint64_t>(h->controls) * 12 != h->raw[0]) {
		if (error)
			*error = "the patch file is damaged";
		return false;
	}
	return true;
}

} // namespace

bool MakePatchBuffers(const std::vector<uint8_t> &oldBytes, const std::vector<uint8_t> &newBytes,
                      std::vector<uint8_t> *patch, std::string *error) {
	if (newBytes.empty()) {
		if (error)
			*error = "the new file is empty";
		return false;
	}
	if (oldBytes.size() > 0x7FFFFFFF || newBytes.size() > 0x7FFFFFFF) {
		if (error)
			*error = "the files are too large for this patch format";
		return false;
	}

	const Index          index(oldBytes);
	const uint8_t       *od = oldBytes.data();
	const uint8_t       *nd = newBytes.data();
	const int64_t        oldSize = static_cast<int64_t>(oldBytes.size());
	const int64_t        newSize = static_cast<int64_t>(newBytes.size());
	std::vector<uint8_t> ctrl, diff, extra;
	uint32_t             controls = 0;

	int64_t scan = 0, len = 0, pos = 0, lastscan = 0, lastpos = 0, lastoffset = 0;
	while (scan < newSize) {
		int64_t oldscore = 0;
		int64_t scsc;
		for (scsc = scan += len; scan < newSize; ++scan) {
			len = index.Search(newBytes, scan, &pos);
			for (; scsc < scan + len; ++scsc)
				if (scsc + lastoffset >= 0 && scsc + lastoffset < oldSize &&
				    od[scsc + lastoffset] == nd[scsc])
					++oldscore;
			if ((len == oldscore && len != 0) || len > oldscore + 8)
				break;
			if (scan + lastoffset >= 0 && scan + lastoffset < oldSize &&
			    od[scan + lastoffset] == nd[scan])
				--oldscore;
		}

		if (len == oldscore && scan != newSize)
			continue;

		// How far the previous alignment is worth carrying forward...
		int64_t s = 0, sf = 0, lenf = 0;
		for (int64_t i = 0; lastscan + i < scan && lastpos + i < oldSize;) {
			if (od[lastpos + i] == nd[lastscan + i])
				++s;
			++i;
			if (s * 2 - i > sf * 2 - lenf) {
				sf   = s;
				lenf = i;
			}
		}

		// ...and how far back the new one is worth starting.
		int64_t lenb = 0;
		if (scan < newSize) {
			int64_t sb = 0;
			s          = 0;
			for (int64_t i = 1; scan >= lastscan + i && pos >= i; ++i) {
				if (od[pos - i] == nd[scan - i])
					++s;
				if (s * 2 - i > sb * 2 - lenb) {
					sb   = s;
					lenb = i;
				}
			}
		}

		if (lastscan + lenf > scan - lenb) {
			const int64_t overlap = (lastscan + lenf) - (scan - lenb);
			int64_t       ss = 0, lens = 0;
			s = 0;
			for (int64_t i = 0; i < overlap; ++i) {
				if (nd[lastscan + lenf - overlap + i] == od[lastpos + lenf - overlap + i])
					++s;
				if (nd[scan - lenb + i] == od[pos - lenb + i])
					--s;
				if (s > ss) {
					ss   = s;
					lens = i + 1;
				}
			}
			lenf += lens - overlap;
			lenb -= lens;
		}

		const int64_t extraLen = (scan - lenb) - (lastscan + lenf);
		for (int64_t i = 0; i < lenf; ++i)
			diff.push_back(static_cast<uint8_t>(nd[lastscan + i] - od[lastpos + i]));
		for (int64_t i = 0; i < extraLen; ++i)
			extra.push_back(nd[lastscan + lenf + i]);

		PutU32(ctrl, static_cast<uint32_t>(lenf));
		PutU32(ctrl, static_cast<uint32_t>(extraLen));
		PutU32(ctrl, static_cast<uint32_t>(static_cast<int32_t>((pos - lenb) - (lastpos + lenf))));
		++controls;

		lastscan   = scan - lenb;
		lastpos    = pos - lenb;
		lastoffset = pos - scan;
	}

	std::vector<uint8_t> packed[3];
	const std::vector<uint8_t> *raw[3] = {&ctrl, &diff, &extra};
	for (int i = 0; i < 3; ++i) {
		if (!Deflate(*raw[i], &packed[i])) {
			if (error)
				*error = "could not compress the patch";
			return false;
		}
	}

	patch->clear();
	patch->insert(patch->end(), kMagic, kMagic + 8);
	PutU32(*patch, static_cast<uint32_t>(oldBytes.size()));
	const std::string oldMd5 = Md5Of(oldBytes);
	patch->insert(patch->end(), oldMd5.begin(), oldMd5.end());
	PutU32(*patch, static_cast<uint32_t>(newBytes.size()));
	const std::string newMd5 = Md5Of(newBytes);
	patch->insert(patch->end(), newMd5.begin(), newMd5.end());
	PutU32(*patch, controls);
	for (int i = 0; i < 3; ++i) {
		PutU32(*patch, static_cast<uint32_t>(raw[i]->size()));
		PutU32(*patch, static_cast<uint32_t>(packed[i].size()));
	}
	for (int i = 0; i < 3; ++i)
		patch->insert(patch->end(), packed[i].begin(), packed[i].end());

	// Never hand out a patch that has not been seen to work.
	std::vector<uint8_t> check;
	if (!ApplyPatchBuffers(oldBytes, *patch, &check, error) || check != newBytes) {
		if (error && error->empty())
			*error = "the patch did not reproduce the new file";
		return false;
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

	Header h;
	if (!ParseHeader(patch, &h, error))
		return false;
	if (oldBytes.size() != h.oldSize)
		return fail("this patch is for a different build: the file is the wrong size");
	if (Md5Of(oldBytes) != h.oldMd5)
		return fail("this patch is for a different build: the file's MD5 does not match");

	std::vector<uint8_t> streams[3];
	const uint8_t       *at = patch.data() + kHeaderSize;
	for (int i = 0; i < 3; ++i) {
		if (!Inflate(at, h.packed[i], h.raw[i], &streams[i]))
			return fail("the patch file is damaged: a section does not unpack");
		at += h.packed[i];
	}
	const std::vector<uint8_t> &ctrl = streams[0], &diff = streams[1], &extra = streams[2];

	newBytes->assign(h.newSize, 0);
	int64_t oldPos = 0, newPos = 0;
	size_t  diffAt = 0, extraAt = 0;
	const int64_t oldSize = static_cast<int64_t>(oldBytes.size());

	for (uint32_t i = 0; i < h.controls; ++i) {
		const uint32_t diffLen  = GetU32(ctrl.data() + i * 12);
		const uint32_t extraLen = GetU32(ctrl.data() + i * 12 + 4);
		const int32_t  seek     = static_cast<int32_t>(GetU32(ctrl.data() + i * 12 + 8));

		if (newPos + diffLen > h.newSize || oldPos + diffLen > oldSize ||
		    diffAt + diffLen > diff.size())
			return fail("the patch reaches past the end of a file");
		for (uint32_t k = 0; k < diffLen; ++k)
			(*newBytes)[newPos + k] = static_cast<uint8_t>(oldBytes[oldPos + k] + diff[diffAt + k]);
		newPos += diffLen;
		oldPos += diffLen;
		diffAt += diffLen;

		if (newPos + extraLen > h.newSize || extraAt + extraLen > extra.size())
			return fail("the patch reaches past the end of a file");
		if (extraLen)
			std::memcpy(newBytes->data() + newPos, extra.data() + extraAt, extraLen);
		newPos += extraLen;
		extraAt += extraLen;

		oldPos += seek;
		if (oldPos < 0 || oldPos > oldSize)
			return fail("the patch reaches past the end of a file");
	}

	if (newPos != h.newSize || diffAt != diff.size() || extraAt != extra.size())
		return fail("the patch produced a file of the wrong size");
	if (Md5Of(*newBytes) != h.newMd5)
		return fail("the patch produced a file with the wrong MD5");
	return true;
}

bool ReadPatchInfoBuffer(const std::vector<uint8_t> &patch, PatchInfo *info, std::string *error) {
	Header h;
	if (!ParseHeader(patch, &h, error))
		return false;
	info->oldSize    = h.oldSize;
	info->oldMd5     = h.oldMd5;
	info->newSize    = h.newSize;
	info->newMd5     = h.newMd5;
	info->controls   = h.controls;
	info->extraBytes = h.raw[2];
	return true;
}

double StoredFraction(const PatchInfo &info) {
	return info.newSize ? static_cast<double>(info.extraBytes) / info.newSize : 0.0;
}

bool ReadPatchInfo(const std::string &patchPath, PatchInfo *info, std::string *error) {
	std::vector<uint8_t> bytes;
	if (!ReadWholeFile(patchPath, &bytes, error))
		return false;
	return ReadPatchInfoBuffer(bytes, info, error);
}

bool MakePatch(const std::string &oldPath, const std::string &newPath,
               const std::string &patchPath, std::string *error) {
	std::vector<uint8_t> oldBytes, newBytes, patch;
	if (!ReadWholeFile(oldPath, &oldBytes, error) || !ReadWholeFile(newPath, &newBytes, error))
		return false;
	if (!MakePatchBuffers(oldBytes, newBytes, &patch, error))
		return false;
	return WriteFileAtomic(patchPath, patch, error);
}

bool ApplyPatch(const std::string &exePath, const std::string &patchPath,
                const std::string &backupPath, std::string *error) {
	std::vector<uint8_t> oldBytes, patch, newBytes;
	if (!ReadWholeFile(exePath, &oldBytes, error) || !ReadWholeFile(patchPath, &patch, error))
		return false;
	if (!ApplyPatchBuffers(oldBytes, patch, &newBytes, error))
		return false;

	// The original goes first, and is read back before anything else happens.
	// Everything after this point can fail without the player losing the exe
	// they started with.
	if (!backupPath.empty()) {
		if (!WriteFileAtomic(backupPath, oldBytes, error))
			return false;
		std::vector<uint8_t> check;
		if (!ReadWholeFile(backupPath, &check, error) || check != oldBytes) {
			if (error)
				*error = "the backup of gta3.exe did not read back the same, so nothing was changed";
			return false;
		}
	}
	return WriteFileAtomic(exePath, newBytes, error);
}

} // namespace coopiii::installer
