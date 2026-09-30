// What a session has done to the campaign, kept on disk between sittings.
//
// Every mission passed (the campaign log, campaignlog.h), every hidden package
// the group has found (session.h, TakenPickup) and the Import/Export and crane
// lists (server.h, OnCarLists). All three only ever grow and all three are
// what a player who is behind needs to catch up, so a server started again
// tomorrow picks them up where they were and hands them to whoever joins.
//
// The file sits next to server.exe as CoopIII-Progress.dat. It is written
// whole, to a temporary file first and then moved over the old one, so a
// server killed halfway through a write leaves the last good copy behind.
//
// Every delta carries the scriptHash of the main.scm it came from and a
// client applies nothing whose hash is not its own (protocol.h,
// CampaignDeltaBody), so the log in the file is keyed per delta. The server
// never sees a main.scm and can't tell one group's from another's before the
// first delta arrives, which is why there is one file and not one per hash.
//
// Broken street objects are not in here. A record lives only while somebody
// is within OBJECT_RECORD_RANGE of it (objectrecords.h), a server that has
// just started has nobody near anything, and a single player save doesn't
// keep a broken lamp post either.
#pragma once

#include "campaignlog.h"

#include <coopiii/protocol.h>

#include <cstdint>
#include <string>
#include <vector>

namespace coopiii {

// More packages than the game has, so a file claiming more is not believed.
constexpr uint32_t PROGRESS_PACKAGES_MAX = 1024;

struct SavedProgress {
	uint32_t                        logId = 0;   // CampaignLog::Id
	std::vector<CampaignLog::Entry> deltas;
	uint32_t                        carLists[CAR_LISTS] = {};
	std::vector<PickupIdent>        packages;    // hidden packages somebody took
};

// The file's bytes, and back. Decode refuses anything that is not exactly
// what Encode writes: another format, a count past the limits, a length that
// doesn't match or a checksum that doesn't.
std::string EncodeProgress(const SavedProgress &progress);
bool        DecodeProgress(const std::string &bytes, SavedProgress *out);

// Next to the executable, as CoopIII-Server.ini is.
std::string ProgressPath();

// The whole file into `out`; false when there is none or it can't be read.
bool ReadProgressFile(const std::string &path, std::string *out);
// `bytes` to `path` through `path`.tmp, flushed and then moved over it.
bool WriteProgressFile(const std::string &path, const std::string &bytes);
// Moves the file out of the way as `path` + `suffix`, over any older one.
// False only when there was a file and it could not be moved.
bool SetProgressFileAside(const std::string &path, const char *suffix);

} // namespace coopiii
