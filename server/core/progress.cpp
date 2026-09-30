#include "progress.h"

#include <cstdio>
#include <cstring>
#include <utility>

#include <io.h>
#include <windows.h>

namespace coopiii {
namespace {

// "CIIIPRG" and the format's number. A build that writes something else
// changes the number, and an older build then leaves the file alone.
constexpr char     kMagic[8]      = {'C', 'I', 'I', 'I', 'P', 'R', 'G', '1'};
constexpr uint32_t kFormat        = 1;

struct Header {
	char     magic[8];
	uint32_t format;
	uint32_t deltaSize;     // sizeof(CampaignDeltaBody) when it was written
	uint32_t identSize;     // sizeof(PickupIdent)
	uint32_t logId;
	uint32_t deltaCount;
	uint32_t packageCount;
	uint32_t carLists[CAR_LISTS];
};

struct DeltaRecord {
	uint8_t           ownerId;
	uint8_t           pad[3];
	CampaignDeltaBody body;
};

uint32_t Fnv1a(const char *data, size_t size) {
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < size; ++i) {
		h ^= static_cast<uint8_t>(data[i]);
		h *= 16777619u;
	}
	return h;
}

template <class T>
void Put(std::string &out, const T &value) {
	out.append(reinterpret_cast<const char *>(&value), sizeof value);
}

template <class T>
bool Take(const std::string &in, size_t *at, T *value) {
	if (in.size() - *at < sizeof *value)
		return false;
	std::memcpy(value, in.data() + *at, sizeof *value);
	*at += sizeof *value;
	return true;
}

} // namespace

std::string EncodeProgress(const SavedProgress &progress) {
	Header h{};
	std::memcpy(h.magic, kMagic, sizeof h.magic);
	h.format       = kFormat;
	h.deltaSize    = sizeof(CampaignDeltaBody);
	h.identSize    = sizeof(PickupIdent);
	h.logId        = progress.logId;
	h.deltaCount   = static_cast<uint32_t>(progress.deltas.size());
	h.packageCount = static_cast<uint32_t>(progress.packages.size());
	for (uint8_t i = 0; i < CAR_LISTS; ++i)
		h.carLists[i] = progress.carLists[i];

	std::string out;
	out.reserve(sizeof h + progress.deltas.size() * sizeof(DeltaRecord) +
	            progress.packages.size() * sizeof(PickupIdent) + 4);
	Put(out, h);
	for (const CampaignLog::Entry &e : progress.deltas) {
		DeltaRecord r{};
		r.ownerId = e.ownerId;
		r.body    = e.body;
		Put(out, r);
	}
	for (const PickupIdent &p : progress.packages)
		Put(out, p);
	Put(out, Fnv1a(out.data(), out.size()));
	return out;
}

bool DecodeProgress(const std::string &bytes, SavedProgress *out) {
	if (bytes.size() < sizeof(Header) + 4)
		return false;
	const size_t body = bytes.size() - 4;
	uint32_t     sum  = 0;
	std::memcpy(&sum, bytes.data() + body, 4);
	if (sum != Fnv1a(bytes.data(), body))
		return false;

	size_t at = 0;
	Header h{};
	Take(bytes, &at, &h);
	if (std::memcmp(h.magic, kMagic, sizeof h.magic) != 0 || h.format != kFormat ||
	    h.deltaSize != sizeof(CampaignDeltaBody) || h.identSize != sizeof(PickupIdent) ||
	    h.deltaCount > CAMPAIGN_LOG_MAX || h.packageCount > PROGRESS_PACKAGES_MAX)
		return false;
	const size_t want = sizeof h + static_cast<size_t>(h.deltaCount) * sizeof(DeltaRecord) +
	                    static_cast<size_t>(h.packageCount) * sizeof(PickupIdent);
	if (want != body)
		return false;

	SavedProgress p;
	p.logId = h.logId;
	for (uint8_t i = 0; i < CAR_LISTS; ++i)
		p.carLists[i] = h.carLists[i];
	p.deltas.reserve(h.deltaCount);
	for (uint32_t i = 0; i < h.deltaCount; ++i) {
		DeltaRecord r{};
		Take(bytes, &at, &r);
		CampaignLog::Entry e;
		e.ownerId = r.ownerId;
		e.body    = r.body;
		p.deltas.push_back(e);
	}
	p.packages.reserve(h.packageCount);
	for (uint32_t i = 0; i < h.packageCount; ++i) {
		PickupIdent ident{};
		Take(bytes, &at, &ident);
		p.packages.push_back(ident);
	}
	*out = std::move(p);
	return true;
}

std::string ProgressPath() {
	char        buf[MAX_PATH] = {0};
	const DWORD n             = GetModuleFileNameA(nullptr, buf, MAX_PATH);
	std::string s(buf, n);
	const size_t slash = s.find_last_of("\\/");
	return (slash == std::string::npos ? std::string() : s.substr(0, slash + 1)) +
	       "CoopIII-Progress.dat";
}

bool ReadProgressFile(const std::string &path, std::string *out) {
	FILE *fh = std::fopen(path.c_str(), "rb");
	if (!fh)
		return false;
	std::string text;
	char        buf[65536];
	size_t      n;
	while ((n = std::fread(buf, 1, sizeof buf, fh)) > 0)
		text.append(buf, n);
	const bool ok = !std::ferror(fh);
	std::fclose(fh);
	if (ok)
		*out = std::move(text);
	return ok;
}

bool WriteProgressFile(const std::string &path, const std::string &bytes) {
	const std::string tmp = path + ".tmp";
	FILE             *fh  = std::fopen(tmp.c_str(), "wb");
	if (!fh)
		return false;
	bool ok = std::fwrite(bytes.data(), 1, bytes.size(), fh) == bytes.size();
	ok      = std::fflush(fh) == 0 && ok;
	// Onto the disk before the rename makes it the file: a rename that lands
	// ahead of the data is how a power cut leaves an empty file behind.
	ok = _commit(_fileno(fh)) == 0 && ok;
	ok = std::fclose(fh) == 0 && ok;
	if (!ok || !MoveFileExA(tmp.c_str(), path.c_str(),
	                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		DeleteFileA(tmp.c_str());
		return false;
	}
	return true;
}

bool SetProgressFileAside(const std::string &path, const char *suffix) {
	if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES)
		return true;
	const std::string aside = path + suffix;
	return MoveFileExA(path.c_str(), aside.c_str(),
	                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

} // namespace coopiii
