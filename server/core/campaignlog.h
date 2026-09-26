// What every mission has left behind in the campaign, in the order it
// happened (protocol.h, C_CampaignDelta; docs/missions.md 5.5). Numbered, so a
// client can ask for what came after the last one it has, and work out
// against its own globals which of them its game still lacks.
//
// Kept for as long as the server runs, and named by a number picked when it
// starts: a client that hears another name is talking to a server that has
// started over, whose numbers mean something else.
#pragma once

#include <coopiii/protocol.h>

#include <chrono>
#include <cstdint>
#include <random>
#include <vector>

namespace coopiii {

class CampaignLog {
public:
	CampaignLog() {
		std::random_device random;
		m_id = random() ^ static_cast<uint32_t>(
		                      std::chrono::steady_clock::now().time_since_epoch().count());
		if (m_id == 0)
			m_id = 1;   // 0 is a client that has never heard of one
	}

	// S_MissionState::campaignLog.
	uint32_t Id() const { return m_id; }

	// Numbers `body` and keeps it. The number is what it was given.
	uint32_t Append(uint8_t ownerId, CampaignDeltaBody body) {
		body.seq = ++m_last;
		Entry e;
		e.ownerId = ownerId;
		e.body    = body;
		m_entries.push_back(e);
		return body.seq;
	}

	// One more part for the running mission `number`, while it has sent
	// fewer than CAMPAIGN_PARTS_PER_MISSION. A new mission starts the count.
	bool TakePartFor(uint16_t number) {
		if (number != m_partsFor || m_partsGen != m_missionGen) {
			m_partsFor = number;
			m_partsGen = m_missionGen;
			m_parts    = 0;
		}
		if (m_parts >= CAMPAIGN_PARTS_PER_MISSION)
			return false;
		++m_parts;
		return true;
	}
	// A mission started: its parts are counted from nothing.
	void MissionStarted() { ++m_missionGen; }

	// Every delta after `seq`, oldest first, as the server sends them.
	std::vector<S_CampaignDelta> Since(uint32_t seq) const {
		std::vector<S_CampaignDelta> out;
		for (const Entry &e : m_entries) {
			if (e.body.seq <= seq)
				continue;
			S_CampaignDelta d{};
			d.hdr.opcode = S_CampaignDelta::OPCODE;
			d.ownerId    = e.ownerId;
			d.body       = e.body;
			out.push_back(d);
		}
		return out;
	}

	uint32_t Last() const { return m_last; }
	size_t   Count() const { return m_entries.size(); }

private:
	struct Entry {
		uint8_t           ownerId = INVALID_PLAYER;
		CampaignDeltaBody body    = {};
	};
	std::vector<Entry> m_entries;
	uint32_t           m_last       = 0;
	uint32_t           m_id         = 1;
	uint16_t           m_partsFor   = MISSION_NONE;
	uint32_t           m_partsGen   = 0;
	uint32_t           m_missionGen = 0;
	uint32_t           m_parts      = 0;
};

} // namespace coopiii
